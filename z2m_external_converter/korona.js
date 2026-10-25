// Внешний конвертер Zigbee2MQTT для «Короны» (ESP32-H2, ARGB-лента).
// Положить в data/external_converters/ и перезапустить Z2M.
// В старых версиях Z2M (1.x) ещё добавить в configuration.yaml:
//   external_converters:
//     - korona.js
//
// Что видно в Z2M / HA:
//   light   — лампа: вкл/выкл, цвет, яркость                  (эндпоинт 10)
//   effect  — выбор эффекта из списка                         (эндпоинт 11)
//
// Сделан по образцу конвертера ARGB-хаба.

const {deviceEndpoints, light} = require('zigbee-herdsman-converters/lib/modernExtend');
const exposes = require('zigbee-herdsman-converters/lib/exposes');
const e = exposes.presets;
const ea = exposes.access;

const EFFECTS = ['Свой цвет', 'Радуга', 'Дыхание', 'Перелив', 'Огонёк', 'Мерцание', 'Ночник', 'Камин', 'Волна', 'Сияние', 'Полиция'];
const EP_EFFECT = 11;
const noResp = {disableDefaultResponse: true};

// ---------- из Z2M в устройство ----------
const tzEffect = {
    key: ['effect'],
    convertSet: async (entity, key, value, meta) => {
        const ep = meta.device.getEndpoint(EP_EFFECT);
        const idx = EFFECTS.indexOf(value);
        if (idx <= 0) {
            await ep.command('genOnOff', 'off', {}, noResp);
        } else {
            await ep.command('genLevelCtrl', 'moveToLevelWithOnOff',
                {level: idx * 25, transtime: 0, optionsMask: 0, optionsOverride: 0}, noResp);
        }
        return {state: {effect: idx < 0 ? EFFECTS[0] : value}};
    },
};

// ---------- из устройства в Z2M (когда меняют с монитора порта) ----------
const fzLevel = {
    cluster: 'genLevelCtrl',
    type: ['attributeReport', 'readResponse'],
    convert: (model, msg) => {
        const lvl = msg.data.currentLevel;
        if (lvl === undefined) return;
        if (msg.endpoint.ID === EP_EFFECT) {
            const idx = Math.max(1, Math.min(EFFECTS.length - 1, Math.round(lvl / 25)));
            return {effect: EFFECTS[idx]};
        }
    },
};

const fzOff = {
    cluster: 'genOnOff',
    type: ['attributeReport', 'readResponse'],
    convert: (model, msg) => {
        if (msg.data.onOff === undefined || msg.data.onOff) return;
        if (msg.endpoint.ID === EP_EFFECT) return {effect: EFFECTS[0]};
    },
};

const definition = {
    zigbeeModel: ['Korona-ARGB'],
    model: 'Korona-ARGB',
    vendor: 'DIY',
    description: 'ARGB-лента «Корона» (ESP32-H2): лампа с цветом и эффекты',
    extend: [
        deviceEndpoints({endpoints: {light: 10, effect_ep: 11}}),
        light({endpointNames: ['light'], color: {modes: ['xy']}, powerOnBehavior: false, effect: false}),
    ],
    fromZigbee: [fzLevel, fzOff],
    toZigbee: [tzEffect],
    exposes: [
        e.enum('effect', ea.STATE_SET, EFFECTS).withDescription('Эффект подсветки'),
    ],
    meta: {multiEndpoint: true},
};

module.exports = definition;
