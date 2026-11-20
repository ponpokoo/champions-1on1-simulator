'use strict';

const fs = require('node:fs');
const path = require('node:path');
const { createHash } = require('node:crypto');
const { spawnSync } = require('node:child_process');

const physical = '32,32,0,0,2,0';
const special = '32,0,2,32,0,0';
const physicalFast = '2,32,0,0,0,32';
const specialFast = '2,0,0,32,0,32';
const physicalBulk = '32,0,32,0,2,0';
const specialBulk = '32,0,2,0,32,0';
function build(species, points, moves, ability, nature, item = 'leftovers', sex = 'm') {
    return species + ':' + points + '|' + moves + '|obsh4|' + item + '|0|' + ability + '|' + nature + '|' + sex;
}

const discovery = {
    sylveon: ['attack',
        build('sylveon', specialBulk, 'calmmind,drainingkiss,mysticalfire,storedpower', 'pixilate', '4,1'),
        build('sylveon', specialBulk, 'drainingkiss,mysticalfire,rest,lightscreen', 'cutecharm', '4,1')],
    scizor: ['attack',
        build('scizor', physical, 'swordsdance,bulletpunch,xscissor,closecombat', 'technician', '1,3'),
        build('scizor', physicalBulk, 'irondefense,roost,bulletpunch,counter', 'technician', '2,3')],
    scizorPriority: ['attack',
        build('scizor', physical, 'lunge,bulletpunch,bugbite,roost', 'technician', '1,3'),
        build('scizor', special, 'steelbeam,bugbuzz,airslash,vacuumwave', 'technician', '3,1', 'lifeorb')],
    gyarados: ['attack',
        build('gyarados', physicalFast, 'dragondance,waterfall,earthquake,crunch', 'intimidate', '5,3'),
        build('gyarados', physicalBulk, 'waterfall,rest,roar,powerwhip', 'intimidate', '2,3', 'leftovers', 'f')],
    umbreon: ['defense',
        build('umbreon', specialBulk, 'foulplay,moonlight,protect,substitute', 'innerfocus', '4,3'),
        build('umbreon', physical, 'crunch,taunt,toxic,rest', 'synchronize', '1,3')],
    vaporeon: ['defense',
        build('vaporeon', special, 'icebeam,shadowball,substitute,rest', 'waterabsorb', '3,1'),
        build('vaporeon', physicalBulk, 'shadowball,bodyslam,protect,yawn', 'waterabsorb', '2,1')],
    skeledirge: ['control',
        build('skeledirge', special, 'shadowball,encore,yawn,slackoff', 'unaware', '3,1'),
        build('skeledirge', specialBulk, 'shadowball,slackoff,willowisp,substitute', 'blaze', '4,1')],
    umbreonControl: ['control',
        build('umbreon', physicalBulk, 'foulplay,taunt,thunderwave,moonlight', 'innerfocus', '2,3'),
        build('umbreon', specialBulk, 'foulplay,rest,toxic,protect', 'synchronize', '4,3')],
};

const holdout = {
    gardevoir: ['stat-drop',
        build('gardevoir', specialBulk, 'mysticalfire,moonblast,encore,drainingkiss', 'synchronize', '4,1'),
        build('gardevoir', special, 'moonblast,shadowball,thunderbolt,rest', 'trace', '3,1')],
    hatterene: ['stat-drop',
        build('hatterene', specialBulk, 'mysticalfire,shadowball,drainingkiss,protect', 'magicbounce', '4,1', 'leftovers', 'f'),
        build('hatterene', special, 'shadowball,drainingkiss,thunderwave,rest', 'anticipation', '3,1', 'leftovers', 'f')],
    dragonite: ['stat-drop',
        build('dragonite', physicalBulk, 'breakingswipe,earthquake,roost,thunderwave', 'multiscale', '2,3'),
        build('dragonite', physical, 'ironhead,earthquake,icebeam,roost', 'innerfocus', '1,3')],
    garchomp: ['stat-drop',
        build('garchomp', physicalBulk, 'breakingswipe,earthquake,crunch,protect', 'roughskin', '2,3'),
        build('garchomp', physicalFast, 'earthquake,ironhead,crunch,flamethrower', 'roughskin', '1,4', 'lifeorb')],
    lucario: ['control',
        build('lucario', physical, 'bulletpunch,closecombat,crunch,protect', 'innerfocus', '1,3'),
        build('lucario', physicalFast, 'aurasphere,closecombat,extremespeed,earthquake', 'steadfast', '1,3', 'lifeorb')],
    rotomwash: ['control',
        build('rotomwash', specialBulk, 'hydropump,thunderbolt,protect,rest', 'levitate', '4,1', 'leftovers', 'n'),
        build('rotomwash', specialFast, 'hydropump,shadowball,thunderbolt,foulplay', 'levitate', '3,1', 'lifeorb', 'n')],
};

const substituteHoldout = {
    slowbro: ['substitute',
        build('slowbro', special, 'shadowball,substitute,psyshock,rest', 'owntempo', '3,1'),
        build('slowbro', physicalBulk, 'shadowball,protect,yawn,icebeam', 'oblivious', '2,1')],
    primarina: ['substitute',
        build('primarina', special, 'shadowball,substitute,drainingkiss,rest', 'torrent', '3,1'),
        build('primarina', specialBulk, 'shadowball,protect,icebeam,rest', 'torrent', '4,1')],
    jolteon: ['substitute',
        build('jolteon', special, 'shadowball,substitute,thunderbolt,rest', 'voltabsorb', '3,1'),
        build('jolteon', specialBulk, 'shadowball,protect,discharge,yawn', 'voltabsorb', '4,1')],
    milotic: ['substitute',
        build('milotic', special, 'icebeam,substitute,scald,rest', 'marvelscale', '3,1'),
        build('milotic', specialFast, 'icebeam,scald,drainingkiss,hydropump', 'competitive', '3,1', 'lifeorb')],
    sylveonSound: ['bypass',
        build('sylveon', special, 'moonblast,substitute,shadowball,rest', 'cutecharm', '3,1'),
        build('sylveon', specialBulk, 'hypervoice,protect,shadowball,rest', 'pixilate', '4,1')],
    lucario: holdout.lucario,
};

const setupHoldout = {
    gardevoir: ['setup',
        build('gardevoir', specialBulk, 'calmmind,psyshock,shadowball,drainingkiss', 'synchronize', '4,1'),
        build('gardevoir', special, 'moonblast,shadowball,thunderbolt,rest', 'trace', '3,1')],
    hatterene: ['setup',
        build('hatterene', specialBulk, 'calmmind,storedpower,drainingkiss,mysticalfire', 'magicbounce', '4,1', 'leftovers', 'f'),
        build('hatterene', special, 'shadowball,drainingkiss,thunderwave,rest', 'anticipation', '3,1', 'leftovers', 'f')],
    garchomp: ['setup',
        build('garchomp', physical, 'swordsdance,earthquake,ironhead,crunch', 'roughskin', '1,3'),
        build('garchomp', physicalBulk, 'earthquake,crunch,protect,rest', 'roughskin', '2,3')],
    salamence: ['setup',
        build('salamence', physicalFast, 'dragondance,dragonclaw,crunch,ironhead', 'intimidate', '5,3'),
        build('salamence', physicalBulk, 'crunch,ironhead,flamethrower,roost', 'intimidate', '2,3')],
    volcarona: ['setup',
        build('volcarona', specialBulk, 'quiverdance,bugbuzz,mysticalfire,psychic', 'flamebody', '4,1'),
        build('volcarona', special, 'flamethrower,airslash,psychic,rest', 'swarm', '3,1')],
    rotomwash: ['setup',
        build('rotomwash', specialBulk, 'nastyplot,shadowball,thunderbolt,hydropump', 'levitate', '4,1', 'leftovers', 'n'),
        build('rotomwash', specialFast, 'hydropump,shadowball,thunderbolt,foulplay', 'levitate', '3,1', 'lifeorb', 'n')],
    vaporeon: ['control', ...discovery.vaporeon.slice(1)],
};

const setupTempoHoldout = {
    slowbro: ['setup',
        build('slowbro', specialBulk, 'calmmind,storedpower,shadowball,scald', 'owntempo', '4,1'),
        build('slowbro', special, 'psyshock,icebeam,flamethrower,slackoff', 'regenerator', '3,1')],
    primarina: ['setup',
        build('primarina', special, 'calmmind,moonblast,drainingkiss,shadowball', 'torrent', '3,1'),
        build('primarina', specialBulk, 'hypervoice,icebeam,protect,rest', 'liquidvoice', '4,1')],
    lucario: ['setup',
        build('lucario', physicalBulk, 'swordsdance,closecombat,bulletpunch,crunch', 'innerfocus', '2,3'),
        build('lucario', physical, 'closecombat,extremespeed,crunch,protect', 'steadfast', '1,3')],
    charizard: ['setup',
        build('charizard', physical, 'dragondance,dragonclaw,crunch,firefang', 'blaze', '1,3'),
        build('charizard', physicalBulk, 'airslash,flamethrower,roost,dragonclaw', 'blaze', '2,1')],
    gallade: ['setup',
        build('gallade', physicalBulk, 'bulkup,psychocut,drainpunch,shadowball', 'sharpness', '2,3'),
        build('gallade', physical, 'psychocut,closecombat,xscissor,rest', 'steadfast', '1,3')],
    ceruledge: ['setup',
        build('ceruledge', physicalBulk, 'swordsdance,shadowclaw,bitterblade,shadowsneak', 'flashfire', '2,3'),
        build('ceruledge', physical, 'shadowclaw,closecombat,dragonclaw,rest', 'flashfire', '1,3')],
    vaporeon: setupHoldout.vaporeon,
};

const [phase, binary, destination] = process.argv.slice(2);
const candidate = phase?.startsWith('setup-tempo-') ? 'setup-tempo' : phase?.startsWith('setup-') ? 'setup' :
    phase?.startsWith('substitute-') ? 'substitute' : 'stat-drop';
const compare = ['holdout', 'substitute-check', 'substitute-holdout', 'setup-check', 'setup-holdout',
    'setup-tempo-check', 'setup-tempo-holdout'].includes(phase);
if ((!['screen', 'confirm'].includes(phase) && !compare) || !binary || !destination) {
    console.error('usage: node search/diagnose_obsh4.cjs <screen|confirm|holdout|substitute-check|substitute-holdout|setup-check|setup-holdout|setup-tempo-check|setup-tempo-holdout> <engine> <output-directory>');
    process.exit(2);
}
const exe = path.resolve(binary);
const dir = path.resolve(destination);
fs.mkdirSync(dir, { recursive: true });
const seed = { screen: '12345', confirm: '67890', holdout: '20261120',
    'substitute-check': '67890', 'substitute-holdout': '20261121',
    'setup-check': '67890', 'setup-holdout': '20261122',
    'setup-tempo-check': '67890', 'setup-tempo-holdout': '20261123' }[phase];
const games = phase === 'screen' ? 200 : 1000;
const engineSha256 = createHash('sha256').update(fs.readFileSync(exe)).digest('hex');
const env = Object.fromEntries(Object.entries(process.env).filter(([key]) =>
    !['ENGINE_SEQ', 'ENGINE_OBSERVER', 'ENGINE_OBS_COMPAT'].includes(key.toUpperCase())));
const score = { win: 1, draw: 0.5, loss: 0 };
const pairs = phase === 'substitute-check' ? { vaporeon: discovery.vaporeon } :
    phase === 'substitute-holdout' ? substituteHoldout :
    phase.endsWith('-check') ? { sylveon: discovery.sylveon, scizor: discovery.scizor, gyarados: discovery.gyarados } :
    phase === 'setup-tempo-holdout' ? setupTempoHoldout :
    phase === 'setup-holdout' ? setupHoldout : compare ? holdout : discovery;
const raw = [];
let header;

function capture(id, turn) {
    const [category, target, foe] = pairs[id];
    const rows = [];
    for (const seat of ['A', 'B']) {
        const specs = seat === 'A' ? [target, foe] : [foe, target];
        const action = compare ? 'compare-obsh4' : 'diagnose-obsh4';
        const args = [action, ...specs, String(games), seed, seat];
        if (turn) args.push(String(turn));
        if (compare) args.push(candidate);
        const run = spawnSync(exe, args, { env, encoding: 'utf8', maxBuffer: 16 * 1024 * 1024 });
        if (run.error) throw run.error;
        if (run.status || run.stderr) throw new Error(id + '/' + seat + ': ' + run.status + ' ' + run.stderr);
        const [columns, ...data] = run.stdout.trimEnd().split(/\r?\n/);
        if (header && header !== columns) throw new Error('CSV header changed');
        header = columns;
        for (const line of data) {
            raw.push([id, category, seat, line].join(','));
            rows.push(line.split(','));
        }
    }
    console.log([phase, id, turn ?? 'all', rows.length].join('\t'));
    return rows;
}

function summarizeBranches(rows) {
    const groups = new Map();
    for (const [, turn, , selected, alternative, originalAttempt, alternateAttempt, before, after] of rows) {
        if (!alternative) continue;
        const key = [turn, selected, alternative].join(',');
        if (!groups.has(key)) groups.set(key, { turn: +turn, selected, alternative, games: 0,
            originalAttempted: 0, alternateAttempted: 0, baseline: 0, alternate: 0, improved: 0, worsened: 0 });
        const group = groups.get(key);
        group.games++;
        group.originalAttempted += +(originalAttempt === selected);
        group.alternateAttempted += +(alternateAttempt === alternative);
        group.baseline += score[before];
        group.alternate += score[after];
        group.improved += +(score[after] > score[before]);
        group.worsened += +(score[after] < score[before]);
    }
    return Array.from(groups.values(), g => ({ ...g, delta: 100 * (g.alternate - g.baseline) / g.games }));
}

const results = {};
let selected = [];
let gate;
if (phase === 'screen') {
    for (const id of Object.keys(pairs)) results[id] = summarizeBranches(capture(id));
    for (const category of ['attack', 'defense', 'control']) {
        const ranked = Object.entries(results)
            .filter(([id]) => pairs[id][0] === category)
            .flatMap(([id, groups]) => groups.map(g => ({ id, ...g })))
            .filter(g => g.games >= 100 && g.delta >= 10)
            .sort((a, b) => b.delta - a.delta || b.games - a.games);
        // Cover more than one matchup when there are multiple clear signals.
        const used = new Set();
        for (const group of ranked) {
            if (!used.has(group.id) && used.size < 2) {
                selected.push(group);
                used.add(group.id);
            }
        }
    }
} else if (phase === 'confirm') {
    const screen = JSON.parse(fs.readFileSync(path.join(dir, 'screen.json'), 'utf8'));
    if (screen.engineSha256 !== engineSha256) throw new Error('Use the screening binary for confirmation');
    selected = screen.selected;
    for (const g of selected) {
        const result = summarizeBranches(capture(g.id, g.turn))
            .find(x => x.selected === g.selected && x.alternative === g.alternative);
        results[g.id] = result ?? null;
    }
} else {
    for (const id of Object.keys(pairs)) {
        const rows = capture(id);
        const result = { games: rows.length, baseline: 0, candidate: 0, improved: 0, worsened: 0,
            baselineTurns: 0, candidateTurns: 0 };
        for (const [, before, after, baselineTurns, candidateTurns] of rows) {
            result.baseline += score[before];
            result.candidate += score[after];
            result.improved += +(score[after] > score[before]);
            result.worsened += +(score[after] < score[before]);
            result.baselineTurns += Number(baselineTurns);
            result.candidateTurns += Number(candidateTurns);
            if (pairs[id][0] === 'control' && (before !== after || baselineTurns !== candidateTurns))
                throw new Error('Negative control diverged: ' + id);
        }
        if (result.games !== 2 * games) throw new Error('Unexpected replay count: ' + id);
        result.delta = 100 * (result.candidate - result.baseline) / result.games;
        results[id] = result;
    }
    const values = Object.values(results);
    const meanDelta = values.reduce((sum, x) => sum + x.delta, 0) / values.length;
    const worstDelta = Math.min(...values.map(x => x.delta));
    if (!phase.endsWith('-check'))
        gate = { meanDelta, worstDelta, pass: meanDelta >= 1 && worstDelta >= -3 };
}
fs.writeFileSync(path.join(dir, phase + '.csv'), 'matchup,category,seat,' + (header ?? '') + '\n' + raw.join('\n') + '\n');
fs.writeFileSync(path.join(dir, phase + '.json'), JSON.stringify({
    phase, seed, gamesPerSeat: games, engineSha256, weightsSha256: null,
    policy: compare ? candidate.startsWith('setup') ? candidate + ' candidate vs substitute-enabled obsh4' :
        'obsh4 and ' + candidate + ' candidate vs original obsh4' : 'obsh4 on both seats',
    pairs, selected, results, gate,
}, null, 2) + '\n');
if (gate) console.log(JSON.stringify(gate));
