import { requestWithLogin, formatJSON, apiUrl } from "./utils";

// Explorers a sent transaction can link to, by chain id as the server reports it.
const EXPLORERS = {
    '0x1': 'https://etherscan.io',
    '0xaa36a7': 'https://sepolia.etherscan.io'
};

const CONFIRM_POLL_MS = 5000;
const CONFIRM_POLL_LIMIT = 120;

// The selected publication's identity and optional unsigned transaction.
// Confirmation is checked against its content hash.
let prepared = null;
let pollTimer = null;
let busy = false;

function byId(id) {
    return document.getElementById(id);
}

function parseJson(text) {
    try {
        return JSON.parse(text);
    } catch {
        return null;
    }
}

function showResponse(code, text) {
    byId('publishResponseCode').textContent = code;
    byId('publishResponseBody').textContent = formatJSON(text);
}

function setStatus(message, { isError = false, txHash = null } = {}) {
    const status = byId('publishStatus');
    status.replaceChildren(document.createTextNode(message));
    status.classList.toggle('is-error', isError);

    const explorer = txHash && EXPLORERS[prepared?.transaction?.chainId];
    if (explorer) {
        const link = document.createElement('a');
        link.href = `${explorer}/tx/${txHash}`;
        link.target = '_blank';
        link.rel = 'noopener';
        link.textContent = 'View transaction';
        status.append(' ', link);
    }
}

function stopPolling() {
    clearTimeout(pollTimer);
    pollTimer = null;
}

export function updatePublishActionAvailability() {
    const txHash = byId('publishTxHash').value.trim();
    byId('btn_preparePublication').disabled = busy || !byId('publishName').value.trim();
    byId('btn_sendPublication').disabled = busy || prepared?.status !== 'prepared' || !!txHash;
    byId('btn_confirmPublication').disabled = busy || pollTimer !== null || !prepared || !/^0x[0-9a-fA-F]{64}$/.test(txHash);
    for (const id of ['publishKind', 'publishName', 'publishTxHash']) {
        byId(id).disabled = busy;
    }
}

function setBusy(value) {
    busy = value;
    updatePublishActionAvailability();
}

export function updatePublicationHash() {
    if (busy) return;
    stopPolling();
    updatePublishActionAvailability();
    if (!prepared) return;
    const txHash = byId('publishTxHash').value.trim();
    if (txHash) {
        setStatus(/^0x[0-9a-fA-F]{64}$/.test(txHash)
            ? 'Press Confirm to check the existing transaction. No transaction will be sent.'
            : 'Enter the full transaction hash: 0x followed by 64 hexadecimal characters.');
    } else if (prepared.status === 'prepared') {
        setStatus(`Ready to send from ${prepared.transaction.from}. It deploys the entity at ${prepared.address}.`);
    } else {
        setStatus('Paste the original transaction hash to check its receipt, or Prepare again to check the publication status.');
    }
}

// Changing what to publish invalidates the prepared transaction.
export function resetPublication() {
    if (busy) return;
    stopPolling();
    prepared = null;
    byId('publishTxHash').value = '';
    showResponse('-', 'Waiting...');
    setStatus('Prepare the publication of the selected entity.');
    updatePublishActionAvailability();
}

export async function preparePublication() {
    if (busy) return;
    stopPolling();
    prepared = null;
    updatePublishActionAvailability();

    const kind = byId('publishKind').value;
    const name = byId('publishName').value.trim();
    if (!name) {
        setStatus('Enter the name of one of your entities.', { isError: true });
        return;
    }

    setBusy(true);
    setStatus('Rebuilding the entity and checking whether it is already published...');
    try {
        const res = await requestWithLogin(apiUrl(`/publish/${kind}/prepare`), {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ name })
        });
        const text = await res.text();
        showResponse(res.status, text);
        const body = parseJson(text);

        if (res.ok && (body?.status === 'prepared' || body?.status === 'published')) {
            prepared = { kind, name, ...body };
            if (body.status === 'published') {
                setStatus(`Already published at ${body.address}. No transaction needs to be sent. Confirm can check the original transaction's receipt.`);
            } else if (byId('publishTxHash').value.trim()) {
                setStatus('Prepared. Press Confirm to check the existing transaction. Preparing does not resend it.');
            } else {
                setStatus(`Ready to send from ${body.transaction.from}. It deploys the entity at ${body.address}.`);
            }
        } else {
            const missing = [...(body?.missing ?? []), ...(body?.mismatched ?? [])];
            const detail = missing.length ? ` Publish these first: ${missing.join(', ')}.` : '';
            setStatus(`${body?.message || `Preparation failed (${res.status}).`}${detail}`, { isError: true });
        }
    } catch (error) {
        showResponse('Error', error.message);
        setStatus(error.message, { isError: true });
    } finally {
        setBusy(false);
    }
}

export async function sendPublication() {
    if (busy || prepared?.status !== 'prepared' || byId('publishTxHash').value.trim()) {
        return;
    }
    if (prepared.deadline * 1000 <= Date.now()) {
        prepared.status = 'expired';
        updatePublishActionAvailability();
        setStatus('The prepared transaction has expired. Prepare again before sending.', { isError: true });
        return;
    }
    if (typeof window.ethereum === 'undefined') {
        setStatus('A browser wallet such as MetaMask is needed to send the transaction.', { isError: true });
        return;
    }

    const { transaction } = prepared;
    let txHash;
    setBusy(true);
    setStatus('Complete the request in your wallet.');
    try {
        // The registry accepts the publication only from its owner, who also pays for it.
        const [account] = await window.ethereum.request({ method: 'eth_requestAccounts' });
        if (!account || account.toLowerCase() !== transaction.from.toLowerCase()) {
            setStatus(`Switch the wallet to ${transaction.from}, the owner of this entity.`, { isError: true });
            return;
        }

        const walletChainId = await window.ethereum.request({ method: 'eth_chainId' });
        if (Number.parseInt(walletChainId, 16) !== Number.parseInt(transaction.chainId, 16)) {
            await window.ethereum.request({
                method: 'wallet_switchEthereumChain',
                params: [{ chainId: transaction.chainId }]
            });
        }

        txHash = await window.ethereum.request({ method: 'eth_sendTransaction', params: [transaction] });
        prepared.status = 'sent';
        byId('publishTxHash').value = txHash;
        setStatus('Sent. Checking whether the publication has been mined...', { txHash });
    } catch (error) {
        setStatus(error?.message || 'The wallet did not send the transaction.', { isError: true });
    } finally {
        setBusy(false);
    }
    if (txHash) await confirmUntilSettled(0);
}

export function confirmPublication() {
    return confirmUntilSettled(0);
}

// Asks the server about the transaction; while it is unmined, asks again a little later.
async function confirmUntilSettled(attempt) {
    if (busy) return;
    stopPolling();
    const txHash = byId('publishTxHash').value.trim();
    if (!prepared || !/^0x[0-9a-fA-F]{64}$/.test(txHash)) {
        updatePublishActionAvailability();
        return;
    }

    setBusy(true);
    setStatus('Checking the transaction receipt...', { txHash });
    try {
        const res = await requestWithLogin(apiUrl(`/publish/${prepared.kind}`), {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ name: prepared.name, content_hash: prepared.content_hash, tx_hash: txHash })
        });
        const text = await res.text();
        showResponse(res.status, text);
        const body = parseJson(text);

        if (res.status === 202) {
            if (attempt + 1 >= CONFIRM_POLL_LIMIT) {
                setStatus('Still not mined. Confirm again later with the same transaction hash.', { txHash });
                return;
            }
            setStatus('Waiting for the transaction to be mined...', { txHash });
            pollTimer = setTimeout(() => confirmUntilSettled(attempt + 1), CONFIRM_POLL_MS);
        } else if (res.status === 201) {
            prepared.status = 'mined';
            setStatus(
                `Publication mined at ${body.address} in block ${body.block_number}. The server's registry listing updates after that block is final.`,
                { txHash });
        } else {
            setStatus(body?.message || `Confirmation failed (${res.status}).`, { isError: true, txHash });
        }
    } catch (error) {
        showResponse('Error', error.message);
        setStatus(error.message, { isError: true });
    } finally {
        setBusy(false);
    }
}
