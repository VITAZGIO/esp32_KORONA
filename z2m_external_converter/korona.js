// Внешний конвертер Zigbee2MQTT для устройства "Корона" (ESP32-H2, DIY).
//
// Кладётся в папку external_converters конфига Z2M, подключается строкой
// в configuration.yaml:
//
//   external_converters:
//     - korona.js
//
// После добавления файла — перезапусти Z2M и заново удали/добавь устройство
// (или Reconfigure из UI), чтобы Z2M пересчитал exposes.
//
// ВНИМАНИЕ: zigbeeModel сопоставляется со строкой, которую отдаёт прошивка
// в поле модели устройства (Basic cluster). Я перечислил несколько строк-
// кандидатов из прошивки на случай, если Z2M прочитает Basic cluster не с
// того эндпоинта, который я предполагаю. Если после установки конвертера
// Z2M всё равно покажет устройство как неопознанное — открой в Z2M лог
// сопряжения (полученные modelID/manufacturerName) и пришли мне, поправлю.

import {onOff} from 'zigbee-herdsman-converters/lib/modernExtend';

const definition = {
    zigbeeModel: [
        'Korona ARGB',
        'Krasnyi',
        'Korona yarche',
        'Korona tishe',
    ],
    model: 'Korona-ARGB',
    vendor: 'DIY',
    description: 'ESP32-H2, ARGB-лента с пресетами цвета (проект «Корона»)',
    extend: [
        onOff({
            endpointNames: [
                'main',
                'krasnyi',
                'oranzhevyi',
                'zheltyi',
                'zelenyi',
                'goluboi',
                'sinii',
                'fioletovyi',
                'rozovyi',
                'teply_belyi',
                'holodnyi_belyi',
                'yarche',
                'tishe',
            ],
        }),
    ],
    endpoint: (device) => {
        return {
            main: 10,
            krasnyi: 11,
            oranzhevyi: 12,
            zheltyi: 13,
            zelenyi: 14,
            goluboi: 15,
            sinii: 16,
            fioletovyi: 17,
            rozovyi: 18,
            teply_belyi: 19,
            holodnyi_belyi: 20,
            yarche: 21,
            tishe: 22,
        };
    },
    meta: {multiEndpoint: true},
};

export default definition;
