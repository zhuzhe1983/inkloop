import test from "node:test";
import assert from "node:assert/strict";

import { generateInkApp, normalizeMapType } from "../app/lib/app-model.ts";

test("地图需求生成独立地图应用，并把精确操作留给预览拖拽", () => {
  const app = generateInkApp("显示公司附近地图，标出入口");
  assert.equal(app.spec.kind, "map");
  assert.equal(app.spec.map.locationMode, "picker");
  assert.equal(app.spec.map.query, "公司");
  assert.equal(app.spec.map.zoomLevel, 19);
  assert.equal(app.spec.map.mapType, 0);
  assert.equal(app.spec.map.style, "balanced");
  assert.equal(app.spec.map.marker, true);
  assert.equal(app.spec.map.showAddress, true);
  assert.equal(app.spec.map.coordinateType, "bd09ll");
  assert.equal(app.spec.map.showCoordinates, false);
  assert.equal(app.spec.map.latitude, undefined);
  assert.equal(app.spec.map.longitude, undefined);
  assert.equal(app.spec.display.border, false);
  assert.equal(app.spec.display.renderMode, "inkloop-text");
});

test("地图模板尊重横版、zoomLevel、坐标与无标记指令", () => {
  const app = generateInkApp("横版城市概览地图，zoomLevel 11，显示经纬度，不要标记");
  assert.equal(app.spec.kind, "map");
  assert.equal(app.spec.orientation, "landscape");
  assert.equal(app.spec.map.zoomLevel, 11);
  assert.equal(app.spec.map.marker, false);
  assert.equal(app.spec.map.showCoordinates, true);
});

test("非地图需求不被地图模板截获", () => {
  assert.equal(generateInkApp("生成本月日历").spec.kind, "calendar");
  assert.equal(generateInkApp("显示上海天气").spec.kind, "weather");
});

test("地图模板识别不显示坐标的否定指令", () => {
  const app = generateInkApp("生成公司北门地图，zoomLevel 18，不显示坐标", "map-no-coordinates");
  assert.equal(app.spec.map.query, "公司北门");
  assert.equal(app.spec.map.showCoordinates, false);
});

test("地图模板能从提示词识别卫星与混合图层", () => {
  assert.equal(generateInkApp("生成西湖卫星地图").spec.map.mapType, 1);
  assert.equal(generateInkApp("公司门口卫星+道路地图").spec.map.mapType, 2);
  assert.equal(generateInkApp("生成校园混合地图").spec.map.mapType, 2);
  assert.equal(generateInkApp("生成普通道路地图").spec.map.mapType, 0);
});

test("normalizeMapType 接受数字与别名", () => {
  assert.equal(normalizeMapType(1), 1);
  assert.equal(normalizeMapType("2"), 2);
  assert.equal(normalizeMapType("satellite"), 1);
  assert.equal(normalizeMapType("hybrid"), 2);
  assert.equal(normalizeMapType("nope", 0), 0);
});
