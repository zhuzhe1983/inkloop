import assert from "node:assert/strict";
import test from "node:test";

import { generateInkApp, resolveCardSpec } from "../app/lib/app-model.ts";

test("卡片需求生成固定竖版卡片，并补全可玩的数值与效果", () => {
  const app = generateInkApp("设计一张星空机械守卫桌游卡牌");

  assert.equal(app.spec.kind, "card");
  assert.equal(app.spec.orientation, "portrait");
  assert.ok(app.spec.card);
  assert.ok(app.spec.card.name.length > 0);
  assert.ok(app.spec.card.level >= 1 && app.spec.card.level <= 12);
  assert.ok(app.spec.card.attack >= 0 && app.spec.card.attack <= 9999);
  assert.ok(app.spec.card.defense >= 0 && app.spec.card.defense <= 9999);
  assert.ok(app.spec.card.description.length > 10);
  assert.equal(app.spec.artwork?.layout, "hero");
});

test("用户明确给出的卡片字段覆盖自动生成结果", () => {
  const card = resolveCardSpec(
    "做一张闪卡，卡名：晨星旅者，类型：机械 · 先锋，等级：9，ATK：4321，DEF：2100，效果描述：每次刷新后抽一张灵感牌，并为相邻同伴增加 200 点守护。",
    {
      rarity: "common",
      name: "模型名字",
      type: "模型类型",
      level: 2,
      attack: 100,
      defense: 200,
      description: "模型描述",
      cardId: "MODEL-1",
    },
  );

  assert.equal(card.rarity, "holo");
  assert.equal(card.name, "晨星旅者");
  assert.equal(card.type, "机械 · 先锋");
  assert.equal(card.level, 9);
  assert.equal(card.attack, 4321);
  assert.equal(card.defense, 2100);
  assert.equal(card.description, "每次刷新后抽一张灵感牌，并为相邻同伴增加 200 点守护");
});

test("四种稀有度只改变材质语义，不改变主体坐标默认值", () => {
  for (const [word, rarity] of [["普卡", "common"], ["银卡", "silver"], ["金卡", "gold"], ["闪卡", "holo"]]) {
    const card = generateInkApp(`做一张${word}卡牌`).spec.card;
    assert.equal(card?.rarity, rarity);
    assert.equal(card?.subjectScale, 1);
    assert.equal(card?.subjectX, 0);
    assert.equal(card?.subjectY, 0);
  }
});
