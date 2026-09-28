import { requestWithLogin, formatJSON, apiUrl } from "./utils";

// Explorers a sent transaction can link to, by chain id as the server reports it.
const EXPLORERS = {
    '0x1': 'https://etherscan.io',
    '0xaa36a7': 'https://sepolia.etherscan.io'
};

const CONFIRM_POLL_MS = 5000;
const CONFIRM_POLL_LIMIT = 120;

// The last "prepared" answer: the wallet sends exactly its transaction, and confirmation
// is checked against its content hash.
let prepared = null;
let pollTimer = null;

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

    const explorer = txHash && prepared && EXPLORERS[prepared.transaction.chainId];
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
    byId('btn_sendPublication').disabled = !prepared;
    byId('btn_confirmPublication').disabled = !prepared || !byId('publishTxHash').value.trim();
}

// Changing what to publish invalidates the prepared transaction.
export function resetPublication() {
    stopPolling();
    prepared = null;
    updatePublishActionAvailability();
}

export async function preparePublication() {
    resetPublication();

    const kind = byId('publishKind').value;
    const name = byId('publishName').value.trim();
    if (!name) {
        setStatus('Enter the name of one of your entities.', { isError: true });
        return;
    }

    setStatus('Rebuilding the entity and preparing its transaction...');
    try {
        const res = await requestWithLogin(apiUrl(`/publish/${kind}/prepare`), {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ name })
        });
        const text = await res.text();
        showResponse(res.status, text);
        const body = parseJson(text);

        if (res.ok && body?.status === 'prepared') {
            prepared = { kind, name, ...body };
            setStatus(`Ready to send from ${body.transaction.from}. It deploys the entity at ${body.address}.`);
        } else if (res.ok && body?.status === 'published') {
            setStatus(`Already published at ${body.address}.`);
        } else {
            const missing = [...(body?.missing ?? []), ...(body?.mismatched ?? [])];
            const detail = missing.length ? ` Publish these first: ${missing.join(', ')}.` : '';
            setStatus(`${body?.message || `Preparation failed (${res.status}).`}${detail}`, { isError: true });
        }
    } catch (error) {
        showResponse('Error', error.message);
        setStatus(error.message, { isError: true });
    }
    updatePublishActionAvailability();
}

export async function sendPublication() {
    if (!prepared) {
        return;
    }
    if (typeof window.ethereum === 'undefined') {
        setStatus('A browser wallet such as MetaMask is needed to send the transaction.', { isError: true });
        return;
    }

    const { transaction } = prepared;
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

        const txHash = await window.ethereum.request({ method: 'eth_sendTransaction', params: [transaction] });
        byId('publishTxHash').value = txHash;
        updatePublishActionAvailability();
        setStatus('Sent. Waiting for it to be mined...', { txHash });
        await confirmUntilSettled(0);
    } catch (error) {
        setStatus(error?.message || 'The wallet did not send the transaction.', { isError: true });
    }
}

export function confirmPublication() {
    return confirmUntilSettled(0);
}

// Asks the server about the transaction; while it is unmined, asks again a little later.
async function confirmUntilSettled(attempt) {
    stopPolling();
    const txHash = byId('publishTxHash').value.trim();
    if (!prepared || !txHash) {
        return;
    }

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
            setStatus(
                `Published at ${body.address} in block ${body.block_number}. The registry lists it once that block is final.`,
                { txHash });
        } else {
            setStatus(body?.message || `Confirmation failed (${res.status}).`, { isError: true, txHash });
        }
    } catch (error) {
        showResponse('Error', error.message);
        setStatus(error.message, { isError: true });
    }
}
