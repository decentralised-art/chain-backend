// Run from the repository root: node --experimental-vm-modules tests/js/publish.test.mjs
import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import { createContext, SourceTextModule, SyntheticModule } from 'node:vm';

function deferred() {
    let resolve;
    const promise = new Promise(done => { resolve = done; });
    return { promise, resolve };
}

const elements = Object.fromEntries([
    'publishKind', 'publishName', 'publishRelay', 'publishTxHash', 'publishStatus',
    'publishResponseCode', 'publishResponseBody',
    'btn_preparePublication', 'btn_sendPublication', 'btn_confirmPublication'
].map(id => [id, {
    value: '', checked: false, disabled: false, textContent: '',
    classList: { toggle() {} },
    replaceChildren(node) { this.textContent = node.textContent; },
    append() {}
}]));
const requests = [];
const walletCalls = [];
const timers = new Map();
let nextTimer = 0;
const owner = `0x${'42'.repeat(20)}`;
const txHash = `0x${'99'.repeat(32)}`;
const publication = {
    status: 'prepared', address: `0x${'55'.repeat(20)}`, content_hash: `0x${'aa'.repeat(32)}`,
    deadline: Math.floor(Date.now() / 1000) + 3600,
    transaction: { from: owner, to: `0x${'11'.repeat(20)}`, chainId: '0x1', data: '0xab', gas: '0x1234' }
};
const response = (status, body) => ({ status, ok: status >= 200 && status < 300, text: async () => JSON.stringify(body) });
let reply = () => response(200, publication);
let walletReply = () => { throw new Error('Wallet request rejected'); };
const context = createContext({
    document: {
        getElementById: id => elements[id],
        createTextNode: textContent => ({ textContent }),
        createElement: () => ({})
    },
    window: { ethereum: { request: async request => {
        walletCalls.push(request);
        return walletReply(request);
    } } },
    setTimeout: (callback, delay) => {
        assert.equal(delay, 5000);
        timers.set(++nextTimer, callback);
        return nextTimer;
    },
    clearTimeout: id => timers.delete(id)
});
const utils = new SyntheticModule(['requestWithLogin', 'formatJSON', 'apiUrl'], function () {
    this.setExport('requestWithLogin', async (url, options) => {
        requests.push({ url, body: JSON.parse(options.body) });
        return reply();
    });
    this.setExport('formatJSON', text => text);
    this.setExport('apiUrl', path => path);
}, { context });
const source = new SourceTextModule(await readFile(new URL('../../resources/js/publish.js', import.meta.url), 'utf8'), { context });
await source.link(specifier => {
    assert.equal(specifier, './utils');
    return utils;
});
await source.evaluate();
const ui = source.namespace;
const send = elements.btn_sendPublication;
const confirm = elements.btn_confirmPublication;
const prepare = elements.btn_preparePublication;

// Preparation locks the form and admits only one request.
ui.updatePublishActionAvailability();
assert.ok(prepare.disabled && send.disabled && confirm.disabled);
assert.equal(send.textContent, 'Send with wallet');
elements.publishKind.value = 'transformation';
elements.publishName.value = 'Example';
ui.resetPublication();
assert.equal(prepare.disabled, false);
const preparation = deferred();
reply = () => preparation.promise;
const preparing = ui.preparePublication();
assert.ok(prepare.disabled && send.disabled && confirm.disabled);
assert.ok(elements.publishName.disabled && elements.publishKind.disabled && elements.publishTxHash.disabled);
await ui.preparePublication();
assert.equal(requests.length, 1);
assert.deepEqual(requests[0].body, { name: 'Example' });
assert.ok(elements.publishRelay.disabled);
preparation.resolve(response(200, publication));
await preparing;
assert.equal(send.disabled, false);
assert.equal(elements.publishName.disabled, false);
assert.equal(confirm.disabled, true);

// Wallet rejection permits a retry. A wallet request in progress cannot be duplicated.
await ui.sendPublication();
assert.equal(send.disabled, false);
assert.match(elements.publishStatus.textContent, /rejected/);
const broadcast = deferred();
const walletOpened = deferred();
walletReply = ({ method }) => {
    if (method === 'eth_requestAccounts') return [owner];
    if (method === 'eth_chainId') return '0x1';
    assert.equal(method, 'eth_sendTransaction');
    walletOpened.resolve();
    return broadcast.promise;
};
reply = () => response(202, { status: 'pending' });
const sending = ui.sendPublication();
await walletOpened.promise;
assert.ok(send.disabled && prepare.disabled && elements.publishTxHash.disabled);
await ui.sendPublication();
assert.equal(walletCalls.filter(call => call.method === 'eth_sendTransaction').length, 1);
broadcast.resolve(txHash);
await sending;
assert.equal(elements.publishTxHash.value, txHash);
assert.ok(send.disabled && confirm.disabled);
assert.equal(timers.size, 1);
assert.deepEqual(requests.at(-1).body, { name: 'Example', content_hash: publication.content_hash, tx_hash: txHash });

// Editing the hash stops polling; erasing it does not enable a duplicate send.
elements.publishTxHash.value = '';
ui.updatePublicationHash();
assert.equal(timers.size, 0);
assert.ok(send.disabled && confirm.disabled);
await ui.sendPublication();
assert.equal(walletCalls.filter(call => call.method === 'eth_sendTransaction').length, 1);
elements.publishTxHash.value = txHash;
ui.updatePublicationHash();
assert.equal(confirm.disabled, false);

// Receipt requests cannot overlap. Success permits another check, but never another send.
const receipt = deferred();
reply = () => receipt.promise;
const confirming = ui.confirmPublication();
const requestCount = requests.length;
await ui.confirmPublication();
assert.equal(requests.length, requestCount);
assert.ok(confirm.disabled && elements.publishTxHash.disabled);
const mined = { status: 'mined', address: publication.address, block_number: 99 };
receipt.resolve(response(201, mined));
await confirming;
assert.ok(send.disabled && !confirm.disabled);
assert.match(elements.publishStatus.textContent, /mined.*block 99/);
reply = () => response(201, mined);
await ui.confirmPublication();
assert.equal(requests.length, requestCount + 1);
assert.equal(elements.publishResponseCode.textContent, 201);

// Preparing an already-published entity retains enough identity to check its receipt.
reply = () => response(200, { status: 'published', address: publication.address, content_hash: publication.content_hash });
await ui.preparePublication();
assert.equal(elements.publishTxHash.value, txHash);
assert.ok(send.disabled && !confirm.disabled);
reply = () => response(201, mined);
await ui.confirmPublication();
assert.equal(elements.publishResponseCode.textContent, 201);
assert.match(elements.publishStatus.textContent, /mined/);

// A different entity clears the hash and output. Recovery with a pasted hash blocks sending.
elements.publishName.value = 'Another';
ui.resetPublication();
assert.equal(elements.publishTxHash.value, '');
assert.equal(elements.publishResponseCode.textContent, '-');
assert.ok(send.disabled && confirm.disabled);
elements.publishTxHash.value = txHash;
ui.updatePublicationHash();
reply = () => response(200, publication);
await ui.preparePublication();
assert.ok(send.disabled && !confirm.disabled);
elements.publishTxHash.value = '0x1234';
ui.updatePublicationHash();
assert.ok(send.disabled && confirm.disabled);
const beforeInvalid = requests.length;
await ui.confirmPublication();
assert.equal(requests.length, beforeInvalid);
elements.publishTxHash.value = txHash;
ui.updatePublicationHash();

// Pending checks stop at the limit, unlock Confirm, and can be retried after an RPC error.
reply = () => response(202, { status: 'pending' });
await ui.confirmPublication();
for (let attempt = 1; attempt < 120; ++attempt) {
    assert.equal(timers.size, 1);
    const [id, callback] = timers.entries().next().value;
    timers.delete(id);
    await callback();
}
assert.equal(timers.size, 0);
assert.equal(confirm.disabled, false);
assert.match(elements.publishStatus.textContent, /Confirm again later/);
reply = () => { throw new Error('RPC unavailable'); };
await ui.confirmPublication();
assert.equal(confirm.disabled, false);
assert.equal(elements.publishName.disabled, false);
assert.match(elements.publishStatus.textContent, /RPC unavailable/);

// Expired preparations never open the wallet; failed preparation leaves no sendable draft.
ui.resetPublication();
reply = () => response(200, { ...publication, deadline: 1 });
await ui.preparePublication();
const beforeExpired = walletCalls.length;
await ui.sendPublication();
assert.equal(walletCalls.length, beforeExpired);
assert.ok(send.disabled);
assert.match(elements.publishStatus.textContent, /expired/);
reply = () => response(409, { message: 'Dependencies are missing', missing: ['Required'] });
await ui.preparePublication();
assert.ok(send.disabled && confirm.disabled && !prepare.disabled);
assert.match(elements.publishStatus.textContent, /Required/);

// Relay: the wallet signs offline, the server broadcasts, and the wallet never sends.
const signing = { type: '0x2', nonce: '0x7', maxFeePerGas: '0xd09dc300', maxPriorityFeePerGas: '0x59682f00', value: '0x0' };
const rawTx = `0x02${'ab'.repeat(40)}`;
const walletSends = () => walletCalls.filter(call => call.method === 'eth_sendTransaction').length;
const signingWallet = ({ method, params }) => {
    if (method === 'eth_requestAccounts') return [owner];
    if (method === 'eth_chainId') return '0x1';
    assert.equal(method, 'eth_signTransaction');
    // Built inside the module's context, so compared by value rather than by prototype.
    assert.equal(JSON.stringify(params), JSON.stringify([{ ...publication.transaction, ...signing }]));
    return rawTx;
};
elements.publishName.value = 'Relayed';
elements.publishRelay.checked = true;
ui.resetPublication();
assert.equal(send.textContent, 'Sign and relay');
reply = () => response(200, { ...publication, signing });
await ui.preparePublication();
assert.deepEqual(requests.at(-1).body, { name: 'Relayed', relay: true });
assert.equal(send.disabled, false);

const sendsBeforeRelay = walletSends();
walletReply = signingWallet;
reply = () => response(202, { status: 'pending', tx_hash: txHash });
await ui.sendPublication();
const relayRequest = requests.at(-2);
assert.equal(relayRequest.url, '/publish/transformation/send');
assert.deepEqual(relayRequest.body, { name: 'Relayed', content_hash: publication.content_hash, raw_tx: rawTx });
assert.equal(requests.at(-1).url, '/publish/transformation');
assert.equal(elements.publishTxHash.value, txHash);
assert.equal(walletSends(), sendsBeforeRelay);
assert.equal(timers.size, 1);

// A refusal from the server is shown and leaves nothing to confirm.
ui.resetPublication();
reply = () => response(200, { ...publication, signing });
await ui.preparePublication();
reply = () => response(409, { message: 'the account nonce is already used; prepare the publication again' });
const beforeRefusal = requests.length;
await ui.sendPublication();
assert.equal(requests.length, beforeRefusal + 1);
assert.match(elements.publishStatus.textContent, /prepare the publication again/);
assert.equal(elements.publishTxHash.value, '');
assert.equal(timers.size, 0);
assert.equal(send.disabled, false);

// A wallet that can only sign by sending is told to turn relay off; nothing reaches the server.
walletReply = ({ method }) => {
    if (method === 'eth_requestAccounts') return [owner];
    if (method === 'eth_chainId') return '0x1';
    throw Object.assign(new Error('The method "eth_signTransaction" does not exist'), { code: -32601 });
};
const beforeUnsupported = requests.length;
await ui.sendPublication();
assert.equal(requests.length, beforeUnsupported);
assert.match(elements.publishStatus.textContent, /cannot sign a transaction without sending/);
assert.equal(walletSends(), sendsBeforeRelay);

// A preparation without signing fields cannot be relayed.
ui.resetPublication();
reply = () => response(200, publication);
await ui.preparePublication();
walletReply = signingWallet;
const beforeUnsigned = requests.length;
await ui.sendPublication();
assert.equal(requests.length, beforeUnsigned);
assert.match(elements.publishStatus.textContent, /Prepare again/);

console.log('Publication UI checks passed.');
