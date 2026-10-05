import * as exposes from 'zigbee-herdsman-converters/lib/exposes';

const ea = exposes.access;
const e = exposes.presets;
// Cache only reports received in this process; do not infer a live position from stale MQTT state.
const contacts = new WeakMap();
const modeOf = (device) => device.meta?.mdGateSensorMode === 'Üks andur' ? 'Üks andur' : 'Kaks andurit';
function contactValue(value) {
    if (value === true || value === 1) return true;
    if (value === false || value === 0) return false;
    return undefined;
}
function gateState(values, mode) {
    if (typeof values.closed !== 'boolean') return 'Teadmata';
    if (mode === 'Üks andur') return values.closed ? 'Suletud' : 'Avatud';
    if (typeof values.open !== 'boolean') return 'Teadmata';
    if (values.closed && values.open) return 'Andurite viga';
    if (values.closed) return 'Suletud';
    if (values.open) return 'Avatud';
    return 'Vaheasend';
}
function stateFor(device) {
    const sensor_mode = modeOf(device);
    return {sensor_mode, gate_state: gateState(contacts.get(device) || {}, sensor_mode)};
}
async function readContacts(device) {
    // Reads never send a relay command. Both reads must complete before returning a combined result.
    const values = {};
    for (const [id, key] of [[2, 'closed'], [3, 'open']]) {
        if (id === 3 && modeOf(device) === 'Üks andur') continue;
        const data = await device.getEndpoint(id).read('genBinaryInput', ['presentValue']);
        const value = contactValue(data.presentValue);
        if (value !== undefined) values[key] = value;
    }
    contacts.set(device, values);
    return stateFor(device);
}
const definition = {
    zigbeeModel: ['MD-GATE-ZB1'],
    model: 'MD-GATE-ZB1',
    vendor: 'MakeDIY',
    description: 'Gate controller with full-gate and WALK pulses, configurable duration and limit contacts',
    fromZigbee: [{
        cluster: 'genBinaryInput',
        type: ['attributeReport', 'readResponse'],
        convert: (model, msg, publish, options, meta) => {
            const value = contactValue(msg.data.presentValue);
            const key = {2: 'closed', 3: 'open'}[msg.endpoint.ID];
            if (!key || value === undefined) return;
            contacts.set(meta.device, {...(contacts.get(meta.device) || {}), [key]: value});
            return {[key]: value, ...stateFor(meta.device)};
        },
    }, {
        cluster: 'genAnalogOutput',
        type: ['attributeReport', 'readResponse'],
        convert: (model, msg) => {
            if (msg.endpoint.ID === 4 && Number.isFinite(msg.data.presentValue)) {
                return {pulse_duration: msg.data.presentValue};
            }
        },
    }],
    toZigbee: [{
        key: ['sensor_mode'],
        convertSet: async (entity, key, value, meta) => {
            if (!['Üks andur', 'Kaks andurit'].includes(value)) throw new Error('Vali Üks andur või Kaks andurit');
            meta.device.meta ??= {};
            const previous = meta.device.meta.mdGateSensorMode;
            meta.device.meta.mdGateSensorMode = value;
            try { await meta.device.save(); }
            catch (error) { meta.device.meta.mdGateSensorMode = previous; throw error; }
            contacts.delete(meta.device);
            // State stays unknown until fresh readings arrive after a mode change.
            return {state: {sensor_mode: value, gate_state: 'Teadmata', ...await readContacts(meta.device)}};
        },
        convertGet: async (entity, key, meta) => { await readContacts(meta.device); },
    }, {
        key: ['gate_state'],
        convertGet: async (entity, key, meta) => { await readContacts(meta.device); },
    }, {
        key: ['pulse_duration'],
        convertSet: async (entity, key, value, meta) => {
            if (typeof value !== 'number' || !Number.isInteger(value) || value < 0 || value > 1000) {
                throw new Error('pulse_duration must be 0..1000 ms in 1 ms steps');
            }
            await meta.device.getEndpoint(4).write('genAnalogOutput', {presentValue: value});
            // Read back from the device; writes do not always trigger a report.
            await new Promise((resolve) => setTimeout(resolve, 250));
            await meta.device.getEndpoint(4).read('genAnalogOutput', ['presentValue']);
        },
        convertGet: async (entity, key, meta) => {
            await meta.device.getEndpoint(4).read('genAnalogOutput', ['presentValue']);
        },
    }, {
        key: ['pulse'],
        convertSet: async (entity, key, value, meta) => {
            if (value !== 'PRESS') throw new Error('pulse must be PRESS');
            await meta.device.getEndpoint(1).command('genOnOff', 'on', {}, {});
        },
    }, {
        key: ['walk'],
        convertSet: async (entity, key, value, meta) => {
            if (value !== 'PRESS') throw new Error('walk must be PRESS');
            const endpoint = meta.device.getEndpoint(5);
            if (!endpoint) throw new Error('WALK requires firmware 1.7.0 and a fresh interview');
            await endpoint.command('genOnOff', 'on', {}, {});
        },
    }, {
        key: ['closed', 'open'],
        convertGet: async (entity, key, meta) => {
            await meta.device.getEndpoint(key === 'closed' ? 2 : 3).read('genBinaryInput', ['presentValue']);
        },
    }],
    exposes: (device) => [
        e.binary('closed', ea.STATE_GET, true, false).withLabel('Kinni-andur').withDescription('GPIO0 kontakt GND-ga suletud = true'),
        e.binary('open', ea.STATE_GET, true, false).withLabel('Lahti-andur').withDescription('GPIO1 kontakt GND-ga suletud = true'),
        ...(device?.getEndpoint?.(5) ? [e.enum('walk', ea.SET, ['PRESS']).withLabel('Jalakäiguava').withDescription('Relee 2 impulss. TXRX-01: GPIO23 / S2. Koguava ja jalakäiguava kasutavad ühist 1 s pausi.')] : []),
        e.enum('pulse', ea.SET, ['PRESS']).withLabel('Kogu värav').withDescription('One pulse; repeats during pulse and 1 s cooldown ignored'),
        ...(typeof device?.getEndpoint !== 'function' || device.getEndpoint(4) ? [e.numeric('pulse_duration', ea.ALL).withUnit('ms').withValueMin(0).withValueMax(1000).withValueStep(1)
            .withCategory('config').withDescription('Relay pulse duration, saved on device. Factory default 300 ms. Zero disables pulses. Does not activate the relay')] : []),
        e.enum('gate_state', ea.STATE_GET, ['Avatud', 'Suletud', 'Vaheasend', 'Andurite viga', 'Teadmata']).withLabel('Värava olek').withDescription('Ühe anduriga: Suletud = kinni-anduri kontakt suletud; Avatud = pole kinni-asendis. Kahega: Vaheasend = kumbki lõpplüliti pole aktiivne.'),
        e.enum('sensor_mode', ea.ALL, ['Üks andur', 'Kaks andurit']).withLabel('Andurite arv').withCategory('config').withDescription('Üks andur: kinni-andur. Kaks andurit: kinni- ja lahti-asendi andurid. Vaikimisi kaks.'),
    ],
    configure: async (device, coordinatorEndpoint) => {
        const durationEndpoint = device.getEndpoint(4);
        if (durationEndpoint) {
        // Duration uses explicit readback; disable scheduled reports on this cluster.
        for (const [cluster, delta] of [['genAnalogOutput', 100]]) {
            try {
                await durationEndpoint.configureReporting(cluster, [{
                    attribute: 'presentValue', minimumReportInterval: 0, maximumReportInterval: 65535, reportableChange: delta,
                }]);
            } catch (error) {
                // ESP returns FAILURE when terminating an already absent report.
                // Transport/timeouts must still fail configuration.
                if (!String(error).includes("Status 'FAILURE'")) throw error;
            }
        }
        await durationEndpoint.read('genAnalogOutput', ['presentValue']);
        }
        for (const id of [2, 3]) {
            const endpoint = device.getEndpoint(id);
            await endpoint.bind('genBinaryInput', coordinatorEndpoint);
            // Firmware sends contact reports on change; configure reads the initial state.
            // Avoid the SDK's separate scheduled-report path.
            try {
                await endpoint.configureReporting('genBinaryInput', [{
                    attribute: 'presentValue', minimumReportInterval: 0, maximumReportInterval: 65535,
                }]);
            } catch (error) {
                if (!String(error).includes("Status 'FAILURE'")) throw error;
            }
            await endpoint.read('genBinaryInput', ['presentValue']);
        }
        for (const id of [1, 5]) {
        const relayEndpoint = device.getEndpoint(id);
        if (!relayEndpoint) continue;
        try {
            await relayEndpoint.configureReporting('genOnOff', [{
                attribute: 'onOff', minimumReportInterval: 0, maximumReportInterval: 65535,
            }]);
        } catch (error) {
            if (!String(error).includes("Status 'FAILURE'")) throw error;
        }
        }
    },
};

export default definition;
