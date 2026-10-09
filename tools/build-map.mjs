#!/usr/bin/env node
// Builds firmware/hud_receiver/map_data.h from OpenStreetMap data (Overpass).
//
// The receiver keeps the whole Sölden map in flash, so this only has to run
// when the source data should be refreshed:
//
//   node tools/build-map.mjs
//
// It filters piste lines (no areas) and real ski lifts, simplifies geometry,
// computes the downhill bearing of every piste and validates that bearing
// against the Open-Meteo elevation API. Anything whose way direction looks
// reversed is listed in tools/map-report.txt and is NOT flipped automatically.

import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');

const BBOX = { south: 46.93, west: 10.92, north: 47.02, east: 11.10 };
const SIMPLIFY_METERS = 4;
const OVERPASS_TIMEOUT = 90;
const ELEVATION_BATCH = 100;
const REVERSED_TOLERANCE_M = 3;
const FLIP_REVERSED = !process.argv.includes('--no-flip');

const USER_AGENT = 'DisplayTest-map-builder/1.0 (https://github.com/Dasiuss/DisplayTest)';
const OUTPUT_HEADER = path.join(ROOT, 'firmware', 'hud_receiver', 'map_data.h');
const OUTPUT_REPORT = path.join(ROOT, 'tools', 'map-report.txt');
const OUTPUT_PWA_DATA = path.join(ROOT, 'pwa', 'public', 'solden-pistes.json');
const CACHE_DIR = path.join(ROOT, 'tools', 'data');
const OVERPASS_CACHE = path.join(CACHE_DIR, 'overpass-solden.json');
const ELEVATION_CACHE = path.join(CACHE_DIR, 'elevation-solden.json');
const REFRESH = process.argv.includes('--refresh');

const PISTE_TYPES = ['downhill', 'snow_park', 'connection'];
const LIFT_TYPES = [
  'gondola', 'chair_lift', 'cable_car', 'mixed_lift', 'drag_lift', 't-bar',
  'j-bar', 'platter', 'rope_tow', 'magic_carpet', 'funicular',
];
const AERIALWAY_EXCLUDE = new Set(['station', 'pylon', 'stop_position', 'goods', 'explosive', 'yes']);
const LIFECYCLE_PREFIXES = ['proposed', 'construction', 'planned', 'disused', 'abandoned', 'razed', 'removed', 'demolished'];

const DIFFICULTY_CODE = { novice: 0, easy: 1, intermediate: 2, advanced: 3, freeride: 4 };
const DIFFICULTY_FALLBACK = 5;
const KIND_CODE = { downhill: 0, snow_park: 1, connection: 2 };

const OVERPASS_ENDPOINTS = [
  'https://overpass-api.de/api/interpreter',
  'https://overpass.kumi.systems/api/interpreter',
  'https://overpass.private.coffee/api/interpreter',
];

const ORIGIN_LAT = (BBOX.south + BBOX.north) / 2;
const ORIGIN_LON = (BBOX.west + BBOX.east) / 2;
const METERS_PER_DEG_LAT = 110540;
const METERS_PER_DEG_LON = 111320 * Math.cos((ORIGIN_LAT * Math.PI) / 180);

const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

function toMeters(lat, lon) {
  return {
    east: (lon - ORIGIN_LON) * METERS_PER_DEG_LON,
    north: (lat - ORIGIN_LAT) * METERS_PER_DEG_LAT,
  };
}

function bearingDegrees(from, to) {
  const east = to.east - from.east;
  const north = to.north - from.north;
  let bearing = (Math.atan2(east, north) * 180) / Math.PI;
  if (bearing < 0) bearing += 360;
  return bearing;
}

function asciiOnly(value) {
  return value
    .normalize('NFD')
    .replace(/[\u0300-\u036f]/g, '')
    .replace(/[^\x20-\x7E]/g, '')
    .trim();
}

function hasLifecyclePrefix(tags) {
  return Object.keys(tags).some((key) => LIFECYCLE_PREFIXES.some((prefix) => key.startsWith(`${prefix}:`)));
}

async function overpass(query) {
  let lastError;
  for (const endpoint of OVERPASS_ENDPOINTS) {
    for (let attempt = 1; attempt <= 3; attempt += 1) {
      try {
        const response = await fetch(endpoint, {
          method: 'POST',
          headers: {
            'Content-Type': 'application/x-www-form-urlencoded',
            'User-Agent': USER_AGENT,
          },
          body: `data=${encodeURIComponent(query)}`,
        });
        const text = await response.text();
        if (!response.ok || text.includes('Error</strong>') || text.startsWith('<?xml')) {
          throw new Error(`Overpass error from ${endpoint}`);
        }
        return JSON.parse(text);
      } catch (error) {
        lastError = error;
        console.warn(`  overpass attempt ${attempt} on ${endpoint} failed: ${error.message}`);
        await sleep(1500 * attempt);
      }
    }
  }
  throw lastError ?? new Error('Overpass unreachable');
}

// Douglas-Peucker on projected meter coordinates. Keeps the original points.
function simplify(points, tolerance) {
  if (points.length <= 2) return points;
  const keep = new Array(points.length).fill(false);
  keep[0] = true;
  keep[points.length - 1] = true;

  const stack = [[0, points.length - 1]];
  while (stack.length > 0) {
    const [first, last] = stack.pop();
    let maxDistance = -1;
    let index = -1;
    for (let i = first + 1; i < last; i += 1) {
      const distance = perpendicularDistance(points[i], points[first], points[last]);
      if (distance > maxDistance) {
        maxDistance = distance;
        index = i;
      }
    }
    if (maxDistance > tolerance && index !== -1) {
      keep[index] = true;
      stack.push([first, index], [index, last]);
    }
  }
  return points.filter((_, i) => keep[i]);
}

function perpendicularDistance(point, lineStart, lineEnd) {
  const dx = lineEnd.east - lineStart.east;
  const dy = lineEnd.north - lineStart.north;
  const lengthSquared = dx * dx + dy * dy;
  if (lengthSquared === 0) {
    return Math.hypot(point.east - lineStart.east, point.north - lineStart.north);
  }
  const t = ((point.east - lineStart.east) * dx + (point.north - lineStart.north) * dy) / lengthSquared;
  const clamped = Math.max(0, Math.min(1, t));
  return Math.hypot(point.east - (lineStart.east + clamped * dx), point.north - (lineStart.north + clamped * dy));
}

const elevationKey = (point) => `${point.lat.toFixed(6)},${point.lon.toFixed(6)}`;

function loadElevationCache() {
  if (REFRESH || !fs.existsSync(ELEVATION_CACHE)) return new Map();
  try {
    return new Map(Object.entries(JSON.parse(fs.readFileSync(ELEVATION_CACHE, 'utf8'))));
  } catch (error) {
    console.warn(`  could not read elevation cache: ${error.message}`);
    return new Map();
  }
}

async function elevation(points, cache) {
  const result = new Array(points.length).fill(null);
  const missing = [];
  points.forEach((point, index) => {
    const key = elevationKey(point);
    if (cache.has(key)) {
      result[index] = cache.get(key);
    } else {
      missing.push({ index, point, key });
    }
  });

  for (let start = 0; start < missing.length; start += ELEVATION_BATCH) {
    const batch = missing.slice(start, start + ELEVATION_BATCH);
    const latitude = batch.map((item) => item.point.lat.toFixed(6)).join(',');
    const longitude = batch.map((item) => item.point.lon.toFixed(6)).join(',');
    const url = `https://api.open-meteo.com/v1/elevation?latitude=${latitude}&longitude=${longitude}`;
    let data = null;
    for (let attempt = 1; attempt <= 3 && data === null; attempt += 1) {
      try {
        const response = await fetch(url, { headers: { 'User-Agent': USER_AGENT } });
        if (!response.ok) throw new Error(`HTTP ${response.status}`);
        data = await response.json();
      } catch (error) {
        console.warn(`  elevation attempt ${attempt} failed: ${error.message}`);
        await sleep(1200 * attempt);
      }
    }
    if (data && Array.isArray(data.elevation)) {
      batch.forEach((item, i) => {
        const value = data.elevation[i];
        if (typeof value === 'number') {
          result[item.index] = value;
          cache.set(item.key, value);
        }
      });
    }
    await sleep(300);
  }
  return result;
}

function collectWays(elements) {
  const pistes = [];
  const lifts = [];
  const elevationQueries = [];

  for (const element of elements) {
    if (element.type !== 'way' || !Array.isArray(element.geometry) || element.geometry.length < 2) continue;
    const tags = element.tags ?? {};
    if (hasLifecyclePrefix(tags)) continue;

    const pisteTypes = (tags['piste:type'] ?? '').split(';').map((value) => value.trim());
    const isArea = tags.area === 'yes';
    const kind = PISTE_TYPES.find((type) => pisteTypes.includes(type));

    if (kind !== undefined && !isArea) {
      const meters = element.geometry.map((point) => ({ lat: point.lat, lon: point.lon, ...toMeters(point.lat, point.lon) }));
      const simplified = simplify(meters, SIMPLIFY_METERS);
      const downhill = kind === 'downhill';
      const bearing = downhill ? wayBearing(simplified) : -1;
      pistes.push({
        id: element.id,
        name: asciiOnly(tags.name ?? tags.ref ?? '').slice(0, 10),
        fullName: asciiOnly(tags.name ?? tags.ref ?? ''),
        kind,
        difficulty: downhill ? (DIFFICULTY_CODE[tags['piste:difficulty']] ?? DIFFICULTY_FALLBACK) : DIFFICULTY_FALLBACK,
        difficultyName: downhill ? (tags['piste:difficulty'] ?? null) : null,
        bearing,
        points: simplified,
      });
      if (downhill) {
        elevationQueries.push({ piste: pistes.length - 1, start: meters.at(0), end: meters.at(-1) });
      }
      continue;
    }

    const aerialway = (tags.aerialway ?? '').trim();
    if (LIFT_TYPES.includes(aerialway) && !AERIALWAY_EXCLUDE.has(aerialway)) {
      const meters = element.geometry.map((point) => ({ lat: point.lat, lon: point.lon, ...toMeters(point.lat, point.lon) }));
      const simplified = simplify(meters, SIMPLIFY_METERS);
      lifts.push({
        id: element.id,
        name: asciiOnly(tags.name ?? '').slice(0, 3),
        fullName: asciiOnly(tags.name ?? ''),
        type: LIFT_TYPES.indexOf(aerialway),
        typeName: aerialway,
        points: simplified,
      });
    }
  }

  return { pistes, lifts, elevationQueries };
}

function wayBearing(points) {
  let bearing = bearingDegrees(points.at(0), points.at(-1));
  if (Number.isNaN(bearing) || Math.hypot(points.at(-1).east - points[0].east, points.at(-1).north - points[0].north) < 5) {
    bearing = bearingDegrees(points[0], points[Math.min(1, points.length - 1)]);
  }
  return Math.round(bearing) % 360;
}

function buildHeader(pistes, lifts, points, names, nameIndex) {
  const lines = [];
  lines.push('// Generated by tools/build-map.mjs - do not edit by hand.');
  lines.push('#pragma once');
  lines.push('#include <Arduino.h>');
  lines.push('');
  lines.push('struct MapPoint { int16_t east; int16_t north; };');
  lines.push('struct MapWay {');
  lines.push('  uint16_t firstPoint;');
  lines.push('  uint16_t pointCount;');
  lines.push('  uint16_t nameOffset;');
  lines.push('  uint8_t nameLength;');
  lines.push('  uint8_t kind;');
  lines.push('  uint8_t difficulty;');
  lines.push('  int16_t bearing;');
  lines.push('};');
  lines.push('struct MapLift {');
  lines.push('  uint16_t firstPoint;');
  lines.push('  uint16_t pointCount;');
  lines.push('  uint16_t nameOffset;');
  lines.push('  uint8_t nameLength;');
  lines.push('  uint8_t type;');
  lines.push('};');
  lines.push('');
  lines.push(`static const int32_t MAP_ORIGIN_LAT_E7 = ${Math.round(ORIGIN_LAT * 1e7)};`);
  lines.push(`static const int32_t MAP_ORIGIN_LON_E7 = ${Math.round(ORIGIN_LON * 1e7)};`);
  lines.push(`static const uint16_t MAP_POINT_COUNT = ${points.length};`);
  lines.push(`static const uint16_t MAP_WAY_COUNT = ${pistes.length};`);
  lines.push(`static const uint16_t MAP_LIFT_COUNT = ${lifts.length};`);
  lines.push('');
  lines.push('static const MapPoint mapPoints[] PROGMEM = {');
  for (const point of points) {
    lines.push(`  {${point.east}, ${point.north}},`);
  }
  lines.push('};');
  lines.push('');
  lines.push('static const MapWay mapWays[] PROGMEM = {');
  pistes.forEach((piste, index) => {
    const meta = nameIndex(piste.name);
    lines.push(`  {${piste.firstPoint}, ${piste.points.length}, ${meta.offset}, ${meta.length}, ${KIND_CODE[piste.kind]}, ${piste.difficulty}, ${piste.bearing}},`);
  });
  lines.push('};');
  lines.push('');
  lines.push('static const MapLift mapLifts[] PROGMEM = {');
  lifts.forEach((lift) => {
    const meta = nameIndex(lift.name);
    lines.push(`  {${lift.firstPoint}, ${lift.points.length}, ${meta.offset}, ${meta.length}, ${lift.type}},`);
  });
  lines.push('};');
  lines.push('');
  lines.push('static const char mapNames[] PROGMEM =');
  for (const chunk of names) {
    lines.push(`  "${chunk}\\0"`);
  }
  lines.push('  ;');
  lines.push('');
  return lines.join('\n');
}

function buildPwaData(pistes, lifts) {
  const round = (value) => Math.round(value * 1e6) / 1e6;
  const coords = (points) => points.map((point) => [round(point.lat), round(point.lon)]);
  return {
    generated: new Date().toISOString(),
    origin: { lat: round(ORIGIN_LAT), lon: round(ORIGIN_LON) },
    pistes: pistes.map((piste) => ({
      n: piste.fullName,
      d: piste.difficultyName,
      k: piste.kind,
      c: coords(piste.points),
    })),
    lifts: lifts.map((lift) => ({
      n: lift.fullName,
      t: lift.typeName,
      c: coords(lift.points),
    })),
  };
}

async function main() {
  const query = `[out:json][timeout:${OVERPASS_TIMEOUT}];(way["piste:type"](${BBOX.south},${BBOX.west},${BBOX.north},${BBOX.east});way["aerialway"](${BBOX.south},${BBOX.west},${BBOX.north},${BBOX.east}););out geom;`;
  fs.mkdirSync(CACHE_DIR, { recursive: true });

  let data;
  if (!REFRESH && fs.existsSync(OVERPASS_CACHE)) {
    console.log(`Using cached Overpass data ${path.relative(ROOT, OVERPASS_CACHE)} (pass --refresh to re-fetch).`);
    data = JSON.parse(fs.readFileSync(OVERPASS_CACHE, 'utf8'));
  } else {
    console.log('Fetching Overpass data...');
    data = await overpass(query);
    fs.writeFileSync(OVERPASS_CACHE, JSON.stringify(data));
    console.log(`Saved raw Overpass data to ${path.relative(ROOT, OVERPASS_CACHE)}`);
  }
  console.log(`  elements: ${data.elements.length}`);

  const { pistes, lifts, elevationQueries } = collectWays(data.elements);
  console.log(`  pistes: ${pistes.length} (downhill ${pistes.filter((p) => p.kind === 'downhill').length})`);
  console.log(`  lifts: ${lifts.length}`);

  // Flatten points and assign offsets.
  const points = [];
  const assign = (items) => {
    for (const item of items) {
      item.firstPoint = points.length;
      for (const point of item.points) {
        points.push({ east: Math.round(point.east), north: Math.round(point.north) });
      }
    }
  };
  assign(pistes);
  assign(lifts);
  console.log(`  points after simplification: ${points.length}`);

  // Elevation validation (report only, no automatic flip).
  console.log('Validating piste direction with Open-Meteo elevation...');
  const samples = [];
  for (const entry of elevationQueries) {
    samples.push({ lat: entry.start.lat, lon: entry.start.lon }, { lat: entry.end.lat, lon: entry.end.lon });
  }
  const elevationCache = loadElevationCache();
  const heights = await elevation(samples, elevationCache);
  fs.writeFileSync(ELEVATION_CACHE, JSON.stringify(Object.fromEntries(elevationCache)));
  console.log(`  elevation cache entries: ${elevationCache.size}`);

  const report = [];
  report.push('# Piste direction report');
  report.push(`# generated ${new Date().toISOString()}`);
  report.push('# wayId	name	startElev	endElev	drop	wayBearing	suggestedBearing	verdict');
  let reversed = 0;
  let flat = 0;
  let failed = 0;
  elevationQueries.forEach((entry, index) => {
    const piste = pistes[entry.piste];
    const startElev = heights[index * 2];
    const endElev = heights[index * 2 + 1];
    if (startElev === null || endElev === null) {
      failed += 1;
      report.push(`${piste.id}	${piste.name}	?	?	?	${piste.bearing}	?	failed`);
      return;
    }
    const drop = startElev - endElev;
    const originalBearing = piste.bearing;
    let verdict = 'ok';
    if (endElev > startElev + REVERSED_TOLERANCE_M) {
      reversed += 1;
      if (FLIP_REVERSED) {
        piste.bearing = (originalBearing + 180) % 360;
        verdict = 'flipped';
      } else {
        verdict = 'reversed';
      }
    } else if (drop < REVERSED_TOLERANCE_M) {
      verdict = 'flat';
      flat += 1;
    }
    const suggested = (originalBearing + 180) % 360;
    report.push(`${piste.id}	${piste.name}	${startElev.toFixed(1)}	${endElev.toFixed(1)}	${drop.toFixed(1)}	${originalBearing}	${suggested}	${verdict}`);
  });
  report.push(`# SUMMARY: total=${elevationQueries.length} ok=${elevationQueries.length - reversed - flat - failed} reversed=${reversed} flat=${flat} failed=${failed}`);

  // Names table.
  const names = [];
  const nameMap = new Map();
  const nameIndex = (value) => {
    const text = value.slice(0, 10);
    if (text.length === 0) return { offset: 0, length: 0 };
    if (!nameMap.has(text)) {
      const offset = names.reduce((sum, chunk) => sum + chunk.length + 1, 0);
      nameMap.set(text, { offset, length: text.length });
      names.push(text.replace(/\\/g, '\\\\').replace(/"/g, '\\"'));
    }
    return nameMap.get(text);
  };

  const header = buildHeader(pistes, lifts, points, names, nameIndex);
  fs.mkdirSync(path.dirname(OUTPUT_HEADER), { recursive: true });
  fs.writeFileSync(OUTPUT_HEADER, header);
  fs.writeFileSync(OUTPUT_REPORT, report.join('\n'));
  const pwaData = buildPwaData(pistes, lifts);
  fs.writeFileSync(OUTPUT_PWA_DATA, JSON.stringify(pwaData));
  console.log(`Wrote ${path.relative(ROOT, OUTPUT_HEADER)} (${(header.length / 1024).toFixed(1)} KB)`);
  console.log(`Wrote ${path.relative(ROOT, OUTPUT_PWA_DATA)} (${(fs.statSync(OUTPUT_PWA_DATA).size / 1024).toFixed(1)} KB)`);
  console.log(`Wrote ${path.relative(ROOT, OUTPUT_REPORT)} (reversed=${reversed}, flat=${flat}, failed=${failed})`);
  if (reversed > 0) {
    console.log(FLIP_REVERSED
      ? `${reversed} reversed piste(s) flipped to match the elevation.`
      : `${reversed} reversed piste(s) found - see the report.`);
  }
}

main().catch((error) => {
  console.error(error);
  process.exit(1);
});
