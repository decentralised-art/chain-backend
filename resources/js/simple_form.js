import { setAccessToken } from "./auth";
import {
    requestWithLogin,
    formatJSON,
    configureLoginHandler,
    apiUrl,
    normalizeConnectorName,
    parseConnectorDimensions,
    getSortedBindingEntries
} from "./utils";

export { copyTextFromElement } from "./utils";

// --------------------------------------------------------------------------
// Transformation
// --------------------------------------------------------------------------
export async function getTransformation() {
    const name = document.getElementById('in_transformationName').value.trim();

    const responseCodeDiv = document.getElementById('transformationResponseCode');
    const responseBodyDiv = document.getElementById('transformationResponseBody');

    if (!name) {
        alert("Transformation name is required.");
        return;
    }

    const url = apiUrl(`/transformation/${name}`);
    try {
        const res = await fetch(url);
        const text = await res.text();
        responseCodeDiv.textContent = res.status;
        responseBodyDiv.textContent = formatJSON(text);
        populateStructuredTransformation(JSON.parse(text));
    } catch (error) {
        responseCodeDiv.textContent = 'Error';
        responseBodyDiv.textContent = error.message;
    }
}

export function updateTransformationPreview() {
    syncTransformationFormAvailability();
    const name = document.getElementById('in_transformationName').value.trim();
    const sol_src = document.getElementById('POST_transformationCode').value.trim();
    const obj = { name, sol_src };
    document.getElementById('POST_transformationRequestBody').textContent = JSON.stringify(obj, null, 2);
}

export function populateStructuredTransformation(jsonOrObject) {
    const data = typeof jsonOrObject === 'string' ? JSON.parse(jsonOrObject) : jsonOrObject;

    const nameInput = document.getElementById('in_transformationName');
    const codeInput = document.getElementById('POST_transformationCode');

    if (!data || typeof data !== 'object') {
        console.warn("Invalid transformation data:", data);
        return;
    }

    nameInput.value = data.name || '';
    codeInput.value = data.sol_src || '';

    updateTransformationPreview(); // trigger live preview rendering
}

export async function sendStructuredTransformation() {
    const name = document.getElementById('in_transformationName').value.trim();
    const sol_src = document.getElementById('POST_transformationCode').value.trim();

    const responseCodeDiv = document.getElementById('transformationResponseCode');
    const responseBodyDiv = document.getElementById('transformationResponseBody');

    if (!name) {
        alert("Transformation name is required.");
        return;
    }

    try {
        const res = await requestWithLogin(apiUrl('/transformation'), {
            method: 'POST',
            headers: {
                'Content-Type': 'application/json'
            },
            body: JSON.stringify({ name, sol_src }),
        });
        const text = await res.text();
        responseCodeDiv.textContent = res.status;
        responseBodyDiv.textContent = formatJSON(text);
    } catch (error) {
        responseCodeDiv.textContent = 'Error';
        responseBodyDiv.textContent = error.message;
    }
}

// --------------------------------------------------------------------------
// Conditions
// --------------------------------------------------------------------------
export async function getCondition() {
    const name = document.getElementById('in_conditionName').value.trim();

    const responseCodeDiv = document.getElementById('conditionResponseCode');
    const responseBodyDiv = document.getElementById('conditionResponseBody');

    if (!name) {
        alert("Condition name is required.");
        return;
    }

    const url = apiUrl(`/condition/${name}`);
    try {
        const res = await fetch(url);
        const text = await res.text();
        responseCodeDiv.textContent = res.status;
        responseBodyDiv.textContent = formatJSON(text);
        populateStructuredCondition(JSON.parse(text));
    } catch (error) {
        responseCodeDiv.textContent = 'Error';
        responseBodyDiv.textContent = error.message;
    }
}

export function updateConditionPreview() {
    syncConditionFormAvailability();
    const name = document.getElementById('in_conditionName').value.trim();
    const sol_src = document.getElementById('POST_conditionCode').value.trim();
    const obj = { name, sol_src };
    document.getElementById('POST_conditionRequestBody').textContent = JSON.stringify(obj, null, 2);
}

export function populateStructuredCondition(jsonOrObject) {
    const data = typeof jsonOrObject === 'string' ? JSON.parse(jsonOrObject) : jsonOrObject;

    const nameInput = document.getElementById('in_conditionName');
    const codeInput = document.getElementById('POST_conditionCode');

    if (!data || typeof data !== 'object') {
        console.warn("Invalid condition data:", data);
        return;
    }

    nameInput.value = data.name || '';
    codeInput.value = data.sol_src || '';

    updateConditionPreview(); // trigger live preview rendering
}

export async function sendStructuredCondition() {
    const name = document.getElementById('in_conditionName').value.trim();
    const sol_src = document.getElementById('POST_conditionCode').value.trim();

    const responseCodeDiv = document.getElementById('conditionResponseCode');
    const responseBodyDiv = document.getElementById('conditionResponseBody');

    if (!name) {
        alert("Condition name is required.");
        return;
    }

    try {
        const res = await requestWithLogin(apiUrl('/condition'), {
            method: 'POST',
            headers: {
                'Content-Type': 'application/json'
            },
            body: JSON.stringify({ name, sol_src }),
        });
        const text = await res.text();
        responseCodeDiv.textContent = res.status;
        responseBodyDiv.textContent = formatJSON(text);
    } catch (error) {
        responseCodeDiv.textContent = 'Error';
        responseBodyDiv.textContent = error.message;
    }
}

// --------------------------------------------------------------------------
// Connector
// --------------------------------------------------------------------------
function parseIntList(value) {
    if (!value) return [];
    return value.split(',')
        .map(item => item.trim())
        .filter(item => item.length > 0)
        .map(item => Number.parseInt(item, 10))
        .filter(item => !Number.isNaN(item));
}

function parseUint32Input(value) {
    if (typeof value !== 'string') {
        return null;
    }

    const trimmed = value.trim();
    if (!/^\d+$/.test(trimmed)) {
        return null;
    }

    const parsed = Number.parseInt(trimmed, 10);
    if (!Number.isSafeInteger(parsed) || parsed < 0 || parsed > 0xFFFFFFFF) {
        return null;
    }

    return parsed;
}

const connectorDefinitionCache = new Map();
const connectorBindingPointCache = new Map();
const connectorBindingRefreshTimers = new WeakMap();
const connectorBindingRefreshTokens = new WeakMap();
const connectorStaticRiTreeRefreshDelayMs = 250;
let connectorStaticRiTreeRefreshTimer = null;
let connectorStaticRiTreeRefreshToken = 0;
let connectorStaticRiPositionOptions = [];
let connectorStaticRiValidationErrors = [];
const connectorStaticRiOptionLabelMaxLength = 120;
const connectorStaticRiValidationSummaryMaxItems = 10;
const connectorStaticRiFocusHighlightDurationMs = 1500;
const connectorStaticRiFocusHighlightTimers = new WeakMap();
let disabledFieldTooltipElement = null;
let disabledFieldTooltipInitialized = false;
let disabledFieldTooltipTarget = null;

function clearConnectorBindingCaches() {
    connectorDefinitionCache.clear();
    connectorBindingPointCache.clear();
}

function hideDisabledFieldTooltip() {
    if (!disabledFieldTooltipElement) {
        return;
    }

    disabledFieldTooltipElement.classList.remove('is-visible');
    disabledFieldTooltipTarget = null;
}

function positionDisabledFieldTooltip(clientX, clientY) {
    if (!disabledFieldTooltipElement) {
        return;
    }

    const offset = 14;
    const maxX = window.innerWidth - disabledFieldTooltipElement.offsetWidth - 12;
    const maxY = window.innerHeight - disabledFieldTooltipElement.offsetHeight - 12;
    const left = Math.max(12, Math.min(clientX + offset, maxX));
    const top = Math.max(12, Math.min(clientY + offset, maxY));

    disabledFieldTooltipElement.style.left = `${left}px`;
    disabledFieldTooltipElement.style.top = `${top}px`;
}

function showDisabledFieldTooltip(control, clientX, clientY) {
    if (!disabledFieldTooltipElement || !control) {
        return;
    }

    const reason = control.getAttribute('data-disabled-reason');
    if (!reason) {
        hideDisabledFieldTooltip();
        return;
    }

    if (disabledFieldTooltipTarget !== control || disabledFieldTooltipElement.textContent !== reason) {
        disabledFieldTooltipElement.textContent = reason;
        disabledFieldTooltipTarget = control;
    }

    disabledFieldTooltipElement.classList.add('is-visible');
    positionDisabledFieldTooltip(clientX, clientY);
}

function resolveDisabledReasonTarget(eventTarget) {
    if (!(eventTarget instanceof Element)) {
        return null;
    }

    const target = eventTarget.closest('[data-disabled-reason]');
    if (!target) {
        return null;
    }

    const disabled = (
        target instanceof HTMLInputElement ||
        target instanceof HTMLSelectElement ||
        target instanceof HTMLTextAreaElement ||
        target instanceof HTMLButtonElement
    ) && target.disabled;

    return disabled ? target : null;
}

function ensureDisabledFieldTooltip() {
    if (disabledFieldTooltipInitialized || typeof document === 'undefined') {
        return;
    }

    disabledFieldTooltipInitialized = true;

    const tooltipElement = document.createElement('div');
    tooltipElement.className = 'disabled-field-tooltip';
    document.body.appendChild(tooltipElement);
    disabledFieldTooltipElement = tooltipElement;

    document.addEventListener('mousemove', (event) => {
        const target = resolveDisabledReasonTarget(event.target);
        if (!target) {
            hideDisabledFieldTooltip();
            return;
        }

        showDisabledFieldTooltip(target, event.clientX, event.clientY);
    }, true);

    document.addEventListener('scroll', hideDisabledFieldTooltip, true);
    window.addEventListener('blur', hideDisabledFieldTooltip);
    document.addEventListener('keydown', (event) => {
        if (event.key === 'Escape') {
            hideDisabledFieldTooltip();
        }
    });
}

function setControlAvailability(control, enabled, disabledReason) {
    if (!control) {
        return;
    }

    control.disabled = !enabled;
    control.classList.toggle('is-disabled-control', !enabled);

    if (enabled) {
        control.removeAttribute('data-disabled-reason');
        control.title = '';
        if (disabledFieldTooltipTarget === control) {
            hideDisabledFieldTooltip();
        }
        return;
    }

    const reason = typeof disabledReason === 'string' && disabledReason.trim().length > 0
        ? disabledReason.trim()
        : 'This field is disabled.';
    control.setAttribute('data-disabled-reason', reason);
    control.title = reason;
}

function syncTransformationFormAvailability() {
    ensureDisabledFieldTooltip();

    const nameInput = document.getElementById('in_transformationName');
    const sendButton = document.getElementById('btn_sendStructuredTransformation');
    const fetchButton = document.getElementById('btn_getTransformation');
    if (!nameInput) {
        return;
    }

    const hasName = nameInput.value.trim().length > 0;
    setControlAvailability(sendButton, hasName, 'Fill transformation name first.');
    setControlAvailability(fetchButton, hasName, 'Fill transformation name first.');
}

function syncConditionFormAvailability() {
    ensureDisabledFieldTooltip();

    const nameInput = document.getElementById('in_conditionName');
    const sendButton = document.getElementById('btn_sendStructuredCondition');
    const fetchButton = document.getElementById('btn_getCondition');
    if (!nameInput) {
        return;
    }

    const hasName = nameInput.value.trim().length > 0;
    setControlAvailability(sendButton, hasName, 'Fill condition name first.');
    setControlAvailability(fetchButton, hasName, 'Fill condition name first.');
}

export function updateExecuteActionAvailability() {
    ensureDisabledFieldTooltip();

    const nameInput = document.getElementById('executeName');
    const particleCountInput = document.getElementById('executeN');
    const fetchGraphButton = document.getElementById('btn_fetchBeforeExecute');
    const runButtons = [document.getElementById('btn_execute'), document.getElementById('btn_simulate')];
    if (!nameInput || runButtons.some((button) => !button)) {
        return;
    }

    const hasName = nameInput.value.trim().length > 0;
    const hasParticleCount = Boolean(particleCountInput && particleCountInput.value.trim().length > 0);
    setControlAvailability(fetchGraphButton, hasName, 'Fill connector name first.');
    for (const button of runButtons) {
        setControlAvailability(
            button,
            hasName && hasParticleCount,
            hasName ? 'Fill particle count first.' : 'Fill connector name first.'
        );
    }
}

function updateClearConnectorDimensionsAvailability() {
    const clearButton = document.getElementById('btn_clearConnectorDimensions');
    const dimensionsContainer = document.getElementById('connectorDimensionsContainer');
    if (!clearButton || !dimensionsContainer) {
        return;
    }

    const hasDimensions = dimensionsContainer.querySelector('.connector-dimension') !== null;
    setControlAvailability(clearButton, hasDimensions, 'Add at least one dimension first.');
}

function updateClearConnectorStaticRiAvailability() {
    const clearButton = document.getElementById('btn_clearConnectorStaticRi');
    const staticRiContainer = document.getElementById('connectorStaticRiContainer');
    if (!clearButton || !staticRiContainer) {
        return;
    }

    const hasStaticRiEntries = staticRiContainer.querySelector('.connector-static-ri') !== null;
    setControlAvailability(clearButton, hasStaticRiEntries, 'Add at least one static RI first.');
}

function updateAddConnectorStaticRiAvailability() {
    const addButton = document.getElementById('btn_addConnectorStaticRi');
    if (!addButton) {
        return;
    }

    const usedPositions = getUsedConnectorStaticRiPositions();
    const hasAvailablePosition = connectorStaticRiPositionOptions.some(({ position }) => !usedPositions.has(position));
    setControlAvailability(
        addButton,
        hasAvailablePosition,
        connectorStaticRiPositionOptions.length === 0
            ? 'Static RI positions are unavailable.'
            : 'All static RI positions are already used.'
    );
}

function setConnectorStaticRiTreeStatus(message, isError = false) {
    const statusElement = document.getElementById('connectorStaticRiTreeStatus');
    if (!statusElement) {
        return;
    }

    const normalizedMessage = typeof message === 'string' && message.trim().length > 0
        ? message.trim()
        : 'Static RI positions are unavailable.';
    statusElement.textContent = normalizedMessage;
    statusElement.classList.toggle('is-error', Boolean(isError));
}

function truncateConnectorStaticRiOptionLabel(label) {
    const normalizedLabel = typeof label === 'string' ? label.trim() : String(label ?? '');
    if (normalizedLabel.length <= connectorStaticRiOptionLabelMaxLength) {
        return normalizedLabel;
    }

    const prefixLength = Math.max(0, connectorStaticRiOptionLabelMaxLength - 3);
    return `${normalizedLabel.slice(0, prefixLength)}...`;
}

function focusConnectorStaticRiRow(rowIndex) {
    if (!Number.isInteger(rowIndex) || rowIndex <= 0) {
        return;
    }

    const rowElement = document.querySelector(`#connectorStaticRiContainer > .connector-static-ri:nth-of-type(${rowIndex})`);
    if (!(rowElement instanceof HTMLElement)) {
        return;
    }

    triggerConnectorStaticRiRowHighlight(rowElement);

    rowElement.scrollIntoView({
        behavior: 'smooth',
        block: 'center',
        inline: 'nearest'
    });

    const focusTarget = rowElement.querySelector(
        '.connector-static-ri-position, .connector-static-ri-start-point, .connector-static-ri-transform-shift'
    );
    if (focusTarget instanceof HTMLElement) {
        focusTarget.focus({ preventScroll: true });
    }
}

function triggerConnectorStaticRiRowHighlight(rowElement) {
    if (!(rowElement instanceof HTMLElement)) {
        return;
    }

    const existingTimeout = connectorStaticRiFocusHighlightTimers.get(rowElement);
    if (existingTimeout) {
        clearTimeout(existingTimeout);
    }

    rowElement.classList.remove('is-focus-highlight');
    // Force reflow so the CSS animation restarts on repeated clicks.
    void rowElement.offsetWidth;
    rowElement.classList.add('is-focus-highlight');

    const timeoutHandle = setTimeout(() => {
        rowElement.classList.remove('is-focus-highlight');
        connectorStaticRiFocusHighlightTimers.delete(rowElement);
    }, connectorStaticRiFocusHighlightDurationMs);

    connectorStaticRiFocusHighlightTimers.set(rowElement, timeoutHandle);
}

function setConnectorStaticRiValidationStatus(errors = []) {
    const statusElement = document.getElementById('connectorStaticRiValidationStatus');
    if (!statusElement) {
        return;
    }

    if (!Array.isArray(errors) || errors.length === 0) {
        statusElement.textContent = '';
        statusElement.classList.remove('is-error');
        return;
    }

    const normalizedErrors = errors
        .filter((errorMessage) => typeof errorMessage === 'string' && errorMessage.trim().length > 0)
        .map((errorMessage) => errorMessage.trim());
    if (normalizedErrors.length === 0) {
        statusElement.textContent = '';
        statusElement.classList.remove('is-error');
        return;
    }

    statusElement.textContent = '';

    const visibleErrors = normalizedErrors.slice(0, connectorStaticRiValidationSummaryMaxItems);
    for (const errorMessage of visibleErrors) {
        const rowMatch = /^Row\s+(\d+):/.exec(errorMessage);
        const rowIndex = rowMatch ? Number.parseInt(rowMatch[1], 10) : null;

        if (rowIndex !== null && Number.isInteger(rowIndex) && rowIndex > 0) {
            const linkButton = document.createElement('button');
            linkButton.type = 'button';
            linkButton.className = 'connector-static-ri-summary-link';
            linkButton.textContent = `- ${errorMessage}`;
            linkButton.title = `Go to row ${rowIndex}`;
            linkButton.addEventListener('click', () => focusConnectorStaticRiRow(rowIndex));
            statusElement.appendChild(linkButton);
            continue;
        }

        const summaryLine = document.createElement('div');
        summaryLine.className = 'connector-static-ri-summary-line';
        summaryLine.textContent = `- ${errorMessage}`;
        statusElement.appendChild(summaryLine);
    }

    const hiddenErrorsCount = normalizedErrors.length - visibleErrors.length;
    if (hiddenErrorsCount > 0) {
        const summaryLine = document.createElement('div');
        summaryLine.className = 'connector-static-ri-summary-line';
        summaryLine.textContent = `- ... and ${hiddenErrorsCount} more error(s).`;
        statusElement.appendChild(summaryLine);
    }

    statusElement.classList.add('is-error');
}

function setConnectorStaticRiEntryValidation(entryElement, message = '') {
    if (!entryElement) {
        return;
    }

    const errorElement = entryElement.querySelector('.connector-static-ri-error');
    const normalizedMessage = typeof message === 'string' ? message.trim() : '';
    const hasError = normalizedMessage.length > 0;

    entryElement.classList.toggle('has-error', hasError);
    if (!errorElement) {
        return;
    }

    errorElement.textContent = hasError ? normalizedMessage : '';
    errorElement.classList.toggle('is-error', hasError);
}

function setConnectorStaticRiPositionSelectOptions(positionSelect, selectedPosition = '') {
    if (!(positionSelect instanceof HTMLSelectElement)) {
        return;
    }

    const normalizedSelectedPosition = typeof selectedPosition === 'string'
        ? selectedPosition.trim()
        : '';

    positionSelect.innerHTML = '';

    const placeholderOption = document.createElement('option');
    placeholderOption.value = '';
    placeholderOption.textContent = 'Select connector position';
    positionSelect.appendChild(placeholderOption);

    let hasSelectedOption = false;
    connectorStaticRiPositionOptions.forEach(({ position, label }) => {
        const option = document.createElement('option');
        option.value = String(position);
        const compactLabel = truncateConnectorStaticRiOptionLabel(label);
        option.textContent = `${position} - ${compactLabel}`;
        option.title = `${position} - ${label}`;
        positionSelect.appendChild(option);

        if (option.value === normalizedSelectedPosition) {
            hasSelectedOption = true;
        }
    });

    if (normalizedSelectedPosition && !hasSelectedOption) {
        const unresolvedOption = document.createElement('option');
        unresolvedOption.value = normalizedSelectedPosition;
        unresolvedOption.textContent = `${normalizedSelectedPosition} - unresolved position`;
        unresolvedOption.title = unresolvedOption.textContent;
        positionSelect.appendChild(unresolvedOption);
        hasSelectedOption = true;
    }

    positionSelect.value = hasSelectedOption ? normalizedSelectedPosition : '';
    const hasAvailableOption = connectorStaticRiPositionOptions.length > 0 || hasSelectedOption;
    setControlAvailability(
        positionSelect,
        hasAvailableOption,
        'Static RI positions are unavailable.'
    );

    const selectedOption = positionSelect.selectedOptions.length > 0
        ? positionSelect.selectedOptions[0]
        : null;
    positionSelect.title = selectedOption?.title || selectedOption?.textContent || '';
}

function refreshConnectorStaticRiPositionInputs() {
    const positionSelects = document.querySelectorAll('.connector-static-ri-position');
    positionSelects.forEach((positionSelect) => {
        if (!(positionSelect instanceof HTMLSelectElement)) {
            return;
        }
        setConnectorStaticRiPositionSelectOptions(positionSelect, positionSelect.value);
    });
}

function getUsedConnectorStaticRiPositions() {
    const usedPositions = new Set();
    document.querySelectorAll('#connectorStaticRiContainer > .connector-static-ri .connector-static-ri-position').forEach((positionSelect) => {
        if (!(positionSelect instanceof HTMLSelectElement)) {
            return;
        }

        const parsedPosition = parseUint32Input(positionSelect.value);
        if (parsedPosition === null) {
            return;
        }

        usedPositions.add(parsedPosition);
    });
    return usedPositions;
}

function buildDefaultConnectorStaticRi() {
    const usedPositions = getUsedConnectorStaticRiPositions();
    const nextFreePosition = connectorStaticRiPositionOptions.find(({ position }) => !usedPositions.has(position));
    if (!nextFreePosition) {
        return null;
    }

    return {
        position: nextFreePosition.position,
        start_point: 0,
        transformation_shift: 0
    };
}

function updateConnectorConditionArgsAvailability() {
    const conditionNameInput = document.getElementById('in_connectorConditionName');
    const conditionArgsInput = document.getElementById('in_connectorConditionArgs');
    if (!conditionNameInput || !conditionArgsInput) {
        return;
    }

    const hasConditionName = conditionNameInput.value.trim().length > 0;
    setControlAvailability(conditionArgsInput, hasConditionName, 'Fill condition name first.');
}

function updateBindingTargetAvailability(bindingElement) {
    if (!bindingElement) {
        return;
    }

    const slotInput = bindingElement.querySelector('.connector-binding-slot');
    const targetInput = bindingElement.querySelector('.connector-binding-target');
    if (!targetInput) {
        return;
    }

    const hasSelectedSlot = Boolean(slotInput && slotInput.value.trim().length > 0);
    setControlAvailability(targetInput, hasSelectedSlot, 'Select binding point id first.');
}

function updateTransformationArgsAvailability(transformationElement) {
    if (!transformationElement) {
        return;
    }

    const nameInput = transformationElement.querySelector('.connector-transformation-name');
    const argsInput = transformationElement.querySelector('.connector-transformation-args');
    if (!argsInput) {
        return;
    }

    const hasName = Boolean(nameInput && nameInput.value.trim().length > 0);
    setControlAvailability(argsInput, hasName, 'Fill transformation name first.');
}

function syncConnectorFormAvailability() {
    ensureDisabledFieldTooltip();
    updateClearConnectorDimensionsAvailability();
    updateAddConnectorStaticRiAvailability();
    updateClearConnectorStaticRiAvailability();
    updateConnectorConditionArgsAvailability();
    const connectorNameInput = document.getElementById('in_connectorName');
    const connectorSendButton = document.getElementById('btn_sendStructuredConnector');
    const connectorFetchButton = document.getElementById('btn_getConnector');
    const hasConnectorName = Boolean(connectorNameInput && connectorNameInput.value.trim().length > 0);
    const hasStaticRiValidationErrors = connectorStaticRiValidationErrors.length > 0;
    setControlAvailability(
        connectorSendButton,
        hasConnectorName && !hasStaticRiValidationErrors,
        hasConnectorName
            ? connectorStaticRiValidationErrors[0]
            : 'Fill connector name first.'
    );
    setControlAvailability(connectorFetchButton, hasConnectorName, 'Fill connector name first.');

    document
        .querySelectorAll('.connector-bindings > .connector-binding')
        .forEach((bindingElement) => updateBindingTargetAvailability(bindingElement));

    document
        .querySelectorAll('.connector-transformations > .connector-transformation')
        .forEach((transformationElement) => updateTransformationArgsAvailability(transformationElement));
}

async function fetchConnectorDefinition(connectorName) {
    const normalizedName = normalizeConnectorName(connectorName);
    if (!normalizedName) {
        throw new Error('Connector name is empty');
    }

    if (connectorDefinitionCache.has(normalizedName)) {
        return connectorDefinitionCache.get(normalizedName);
    }

    const request = (async () => {
        const res = await fetch(apiUrl(`/connector/${encodeURIComponent(normalizedName)}`));
        if (!res.ok) {
            throw new Error(`Failed to fetch connector '${normalizedName}' (${res.status})`);
        }
        return await res.json();
    })();

    connectorDefinitionCache.set(normalizedName, request);
    try {
        return await request;
    } catch (error) {
        connectorDefinitionCache.delete(normalizedName);
        throw error;
    }
}

async function computeOpenBindingPointLabels(connectorName, visiting = new Set(), memo = new Map()) {
    const normalizedName = normalizeConnectorName(connectorName);
    if (!normalizedName) {
        return [];
    }

    if (memo.has(normalizedName)) {
        return memo.get(normalizedName);
    }

    if (visiting.has(normalizedName)) {
        throw new Error(`Connector cycle detected at '${normalizedName}'`);
    }

    visiting.add(normalizedName);
    try {
        const connector = await fetchConnectorDefinition(normalizedName);
        const resolvedName = normalizeConnectorName(connector?.name) || normalizedName;
        const dimensions = parseConnectorDimensions(connector);

        const labels = [];
        for (let dimId = 0; dimId < dimensions.length; dimId++) {
            const dimension = dimensions[dimId];
            const dimLabel = `${resolvedName}:${dimId}`;

            if (!dimension.composite) {
                labels.push(dimLabel);
                continue;
            }

            const childLabels = await computeOpenBindingPointLabels(dimension.composite, visiting, memo);
            const boundTargetsByChildSlot = new Map();
            getSortedBindingEntries(dimension.bindings).forEach(({ slotId, targetName }) => {
                if (slotId >= childLabels.length) {
                    throw new Error(
                        `Connector '${resolvedName}' has out-of-range binding slot ${slotId} ` +
                        `at dimension ${dimId} (child '${dimension.composite}' exports ${childLabels.length} slots)`
                    );
                }

                if (boundTargetsByChildSlot.has(slotId)) {
                    throw new Error(
                        `Connector '${resolvedName}' has duplicate canonical binding slot ${slotId} ` +
                        `at dimension ${dimId}`
                    );
                }

                boundTargetsByChildSlot.set(slotId, targetName);
            });

            // Export child slots in deterministic order.
            // If a child slot is statically bound, expand it through the binding target's exported slots.
            for (let childSlotId = 0; childSlotId < childLabels.length; childSlotId++) {
                const childLabel = childLabels[childSlotId];
                const boundTargetName = boundTargetsByChildSlot.get(childSlotId);
                if (!boundTargetName) {
                    labels.push(`${dimLabel}/${childLabel}`);
                    continue;
                }

                const boundTargetLabels = await computeOpenBindingPointLabels(boundTargetName, visiting, memo);
                for (const targetLabel of boundTargetLabels) {
                    labels.push(`${dimLabel}/${childLabel}/${targetLabel}`);
                }
            }
        }

        memo.set(normalizedName, labels);
        return labels;
    } finally {
        visiting.delete(normalizedName);
    }
}

async function fetchBindingPointOptions(connectorName) {
    const normalizedName = normalizeConnectorName(connectorName);
    if (!normalizedName) {
        return [];
    }

    if (connectorBindingPointCache.has(normalizedName)) {
        return connectorBindingPointCache.get(normalizedName);
    }

    const request = (async () => {
        const labels = await computeOpenBindingPointLabels(normalizedName, new Set(), new Map());
        return labels.map((label, slotId) => ({ slotId, label }));
    })();

    connectorBindingPointCache.set(normalizedName, request);
    try {
        return await request;
    } catch (error) {
        connectorBindingPointCache.delete(normalizedName);
        throw error;
    }
}

async function buildConnectorStaticRiPositionOptions() {
    const connectorNameInput = document.getElementById('in_connectorName');
    const draftConnectorLookupName = normalizeConnectorName(connectorNameInput ? connectorNameInput.value : '');
    const draftConnectorDisplayName = draftConnectorLookupName || '(draft)';
    const draftDimensions = parseConnectorDimensions({ dimensions: parseConnectorDimensionsFromForm() });
    const staticRiPositionOptions = [];
    const connectorContextDefinitionCache = new Map();
    const connectorOpenSlotsCache = new Map();
    let positionCounter = 0;

    const draftConnectorDescriptor = {
        resolvedName: draftConnectorDisplayName,
        dimensions: draftDimensions,
        resolved: true,
        cacheKey: '__draft__'
    };

    function createUnresolvedDescriptor(name) {
        const normalizedName = normalizeConnectorName(name);
        return {
            resolvedName: normalizedName || '(unknown)',
            dimensions: [],
            resolved: false,
            cacheKey: `connector:${(normalizedName || '(unknown)').toLowerCase()}`
        };
    }

    async function resolveConnectorDescriptor(connectorName) {
        const normalizedConnectorName = normalizeConnectorName(connectorName);
        if (!normalizedConnectorName) {
            return createUnresolvedDescriptor(connectorName);
        }

        if (
            draftConnectorLookupName &&
            normalizedConnectorName.toLowerCase() === draftConnectorLookupName.toLowerCase()
        ) {
            return draftConnectorDescriptor;
        }

        const lookupKey = normalizedConnectorName.toLowerCase();
        if (connectorContextDefinitionCache.has(lookupKey)) {
            return connectorContextDefinitionCache.get(lookupKey);
        }

        try {
            const connector = await fetchConnectorDefinition(normalizedConnectorName);
            const resolvedName = normalizeConnectorName(connector?.name) || normalizedConnectorName;
            const descriptor = {
                resolvedName,
                dimensions: parseConnectorDimensions(connector),
                resolved: true,
                cacheKey: `connector:${lookupKey}`
            };
            connectorContextDefinitionCache.set(lookupKey, descriptor);
            return descriptor;
        } catch {
            const unresolvedDescriptor = createUnresolvedDescriptor(normalizedConnectorName);
            connectorContextDefinitionCache.set(lookupKey, unresolvedDescriptor);
            return unresolvedDescriptor;
        }
    }

    async function computeOpenSlots(descriptor, visiting = new Set()) {
        if (!descriptor || typeof descriptor !== 'object') {
            return { openSlots: 1, fullyResolved: false };
        }

        if (connectorOpenSlotsCache.has(descriptor.cacheKey)) {
            return connectorOpenSlotsCache.get(descriptor.cacheKey);
        }

        if (visiting.has(descriptor.cacheKey)) {
            throw new Error(`Connector cycle detected at '${descriptor.resolvedName}'`);
        }

        if (!descriptor.resolved) {
            const unresolvedResult = { openSlots: 1, fullyResolved: false };
            connectorOpenSlotsCache.set(descriptor.cacheKey, unresolvedResult);
            return unresolvedResult;
        }

        visiting.add(descriptor.cacheKey);
        try {
            let openSlots = 0;
            let fullyResolved = true;

            for (let dimId = 0; dimId < descriptor.dimensions.length; dimId++) {
                const dimension = descriptor.dimensions[dimId];
                if (!dimension.composite) {
                    openSlots += 1;
                    continue;
                }

                const childDescriptor = await resolveConnectorDescriptor(dimension.composite);
                const childOpenSlotsInfo = await computeOpenSlots(childDescriptor, visiting);
                openSlots += childOpenSlotsInfo.openSlots;
                if (!childOpenSlotsInfo.fullyResolved) {
                    fullyResolved = false;
                }

                const boundTargetsByChildSlot = new Map();
                getSortedBindingEntries(dimension.bindings).forEach(({ slotId, targetName }) => {
                    if (childOpenSlotsInfo.fullyResolved && slotId >= childOpenSlotsInfo.openSlots) {
                        throw new Error(
                            `Connector '${descriptor.resolvedName}' has out-of-range binding slot ${slotId} ` +
                            `at dimension ${dimId} (child '${dimension.composite}' exports ${childOpenSlotsInfo.openSlots} slots)`
                        );
                    }

                    if (boundTargetsByChildSlot.has(slotId)) {
                        throw new Error(
                            `Connector '${descriptor.resolvedName}' has duplicate canonical binding slot ${slotId} ` +
                            `at dimension ${dimId}`
                        );
                    }

                    boundTargetsByChildSlot.set(slotId, targetName);
                });

                for (const targetName of boundTargetsByChildSlot.values()) {
                    const targetDescriptor = await resolveConnectorDescriptor(targetName);
                    const targetOpenSlotsInfo = await computeOpenSlots(targetDescriptor, visiting);
                    openSlots += targetOpenSlotsInfo.openSlots;
                    if (!targetOpenSlotsInfo.fullyResolved) {
                        fullyResolved = false;
                    }
                }

                openSlots -= boundTargetsByChildSlot.size;
            }

            const result = { openSlots, fullyResolved };
            connectorOpenSlotsCache.set(descriptor.cacheKey, result);
            return result;
        } finally {
            visiting.delete(descriptor.cacheKey);
        }
    }

    function getSortedIncomingBindingEntries(bindings) {
        const entries = [];
        for (const [slotId, binding] of bindings.entries()) {
            if (!Number.isInteger(slotId) || slotId < 0) {
                continue;
            }

            if (!binding || typeof binding !== 'object') {
                continue;
            }

            const targetName = typeof binding.targetName === 'string' ? binding.targetName.trim() : '';
            if (!targetName) {
                continue;
            }

            const fromSlot = Number.isInteger(binding.fromSlot) ? binding.fromSlot : slotId;
            const kind = binding.kind === 'static' ? 'static' : 'forwarded';
            const forwarded = binding.forwarded instanceof Map ? binding.forwarded : new Map();
            entries.push([slotId, { targetName, fromSlot, kind, forwarded }]);
        }

        entries.sort((lhs, rhs) => lhs[0] - rhs[0]);
        return entries;
    }

    function cloneBindingMap(bindingMap) {
        const cloned = new Map();
        if (!(bindingMap instanceof Map)) {
            return cloned;
        }

        for (const [slotId, binding] of bindingMap.entries()) {
            if (!Number.isInteger(slotId) || slotId < 0) {
                continue;
            }

            const clonedBinding = cloneBindingDescriptor(binding, slotId);
            if (!clonedBinding) {
                continue;
            }

            cloned.set(slotId, clonedBinding);
        }

        return cloned;
    }

    function cloneBindingDescriptor(binding, fallbackSlotId = 0) {
        if (!binding || typeof binding !== 'object') {
            return null;
        }

        const targetName = typeof binding.targetName === 'string' ? binding.targetName.trim() : '';
        if (!targetName) {
            return null;
        }

        const fromSlot = Number.isInteger(binding.fromSlot) ? binding.fromSlot : fallbackSlotId;
        const kind = binding.kind === 'static' ? 'static' : 'forwarded';
        return {
            targetName,
            kind,
            fromSlot,
            forwarded: cloneBindingMap(binding.forwarded)
        };
    }

    function cloneBindingAsForwarded(binding, fallbackSlotId = 0) {
        const cloned = cloneBindingDescriptor(binding, fallbackSlotId);
        if (!cloned) {
            return null;
        }

        cloned.kind = 'forwarded';
        return cloned;
    }

    async function expandConnector({
        descriptor,
        parentPath,
        incomingBindings,
        boundDescriptor,
        isRoot = false
    }) {
        const fullPath = `${parentPath}/${descriptor.resolvedName}`;
        const positionId = positionCounter++;

        if (isRoot) {
            staticRiPositionOptions.push({
                position: positionId,
                label: `Root connector ${fullPath}`
            });
        } else if (boundDescriptor) {
            staticRiPositionOptions.push({
                position: positionId,
                label: `Bound connector ${fullPath} (${boundDescriptor.slotLabel})${descriptor.resolved ? '' : ' (unresolved)'}`
            });
        } else {
            staticRiPositionOptions.push({
                position: positionId,
                label: `Connector ${fullPath}${descriptor.resolved ? '' : ' (unresolved)'}`
            });
        }

        if (!descriptor.resolved) {
            return;
        }

        let openSlotId = 0;
        for (let dimId = 0; dimId < descriptor.dimensions.length; dimId++) {
            const dimension = descriptor.dimensions[dimId];
            if (!dimension.composite) {
                const replacement = incomingBindings.get(openSlotId);
                if (replacement) {
                    const replacementDescriptor = await resolveConnectorDescriptor(replacement.targetName);
                    const slotLabel = replacement.kind === 'forwarded'
                        ? `slot ${openSlotId} (forwarded from ${replacement.fromSlot})`
                        : `slot ${openSlotId} (static)`;

                    await expandConnector({
                        descriptor: replacementDescriptor,
                        parentPath: `${fullPath}:${dimId}`,
                        incomingBindings: cloneBindingMap(replacement.forwarded),
                        boundDescriptor: {
                            kind: replacement.kind,
                            slotLabel
                        },
                        isRoot: false
                    });
                } else {
                    staticRiPositionOptions.push({
                        position: positionCounter++,
                        label: `Scalar ${fullPath}:${dimId}`
                    });
                }

                openSlotId += 1;
                continue;
            }

            const childDescriptor = await resolveConnectorDescriptor(dimension.composite);
            const childOpenSlotsInfo = await computeOpenSlots(childDescriptor, new Set());
            const childOpenSlots = childOpenSlotsInfo.openSlots;

            const staticBindingMap = new Map();
            getSortedBindingEntries(dimension.bindings).forEach(({ slotId, targetName }) => {
                if (childOpenSlotsInfo.fullyResolved && slotId >= childOpenSlots) {
                    throw new Error(
                        `Connector '${descriptor.resolvedName}' has out-of-range binding slot ${slotId} ` +
                        `at dimension ${dimId} (child '${dimension.composite}' exports ${childOpenSlots} slots)`
                    );
                }

                if (staticBindingMap.has(slotId)) {
                    throw new Error(
                        `Connector '${descriptor.resolvedName}' has duplicate canonical binding slot ${slotId} ` +
                        `at dimension ${dimId}`
                    );
                }

                staticBindingMap.set(slotId, targetName);
            });

            const slotProjectedStarts = new Array(childOpenSlots).fill(0);
            const slotProjectedWidths = new Array(childOpenSlots).fill(0);
            const slotStaticTargets = new Array(childOpenSlots).fill('');
            const slotSelectedBindings = new Array(childOpenSlots).fill(null);
            const slotForwardedInternalBindings = Array.from({ length: childOpenSlots }, () => new Map());
            let childOpenSlotsInParent = 0;

            for (let childSlotId = 0; childSlotId < childOpenSlots; childSlotId++) {
                slotProjectedStarts[childSlotId] = childOpenSlotsInParent;

                const staticTargetName = staticBindingMap.get(childSlotId);
                if (staticTargetName) {
                    const staticTargetDescriptor = await resolveConnectorDescriptor(staticTargetName);
                    const staticTargetOpenSlotsInfo = await computeOpenSlots(staticTargetDescriptor, new Set());

                    slotStaticTargets[childSlotId] = staticTargetName;
                    slotSelectedBindings[childSlotId] = {
                        targetName: staticTargetName,
                        kind: 'static',
                        fromSlot: childSlotId,
                        forwarded: new Map()
                    };
                    slotProjectedWidths[childSlotId] = staticTargetOpenSlotsInfo.openSlots;
                    childOpenSlotsInParent += staticTargetOpenSlotsInfo.openSlots;
                    continue;
                }

                slotProjectedWidths[childSlotId] = 1;
                childOpenSlotsInParent += 1;
            }

            for (const [parentSlotId, parentBinding] of getSortedIncomingBindingEntries(incomingBindings)) {
                if (parentSlotId < openSlotId) {
                    continue;
                }

                const localSlotId = parentSlotId - openSlotId;
                if (localSlotId >= childOpenSlotsInParent) {
                    continue;
                }

                for (let childSlotId = 0; childSlotId < childOpenSlots; childSlotId++) {
                    const rangeStart = slotProjectedStarts[childSlotId];
                    const rangeWidth = slotProjectedWidths[childSlotId];
                    if (!Number.isInteger(rangeWidth) || rangeWidth <= 0) {
                        continue;
                    }

                    const rangeEndExclusive = rangeStart + rangeWidth;
                    if (localSlotId < rangeStart || localSlotId >= rangeEndExclusive) {
                        continue;
                    }

                    const staticTarget = slotStaticTargets[childSlotId];
                    if (!staticTarget) {
                        const forwardedBinding = cloneBindingAsForwarded(parentBinding, parentSlotId);
                        if (forwardedBinding) {
                            slotSelectedBindings[childSlotId] = forwardedBinding;
                        }
                        break;
                    }

                    const offset = localSlotId - rangeStart;
                    const forwardedInternalBinding = cloneBindingAsForwarded(parentBinding, parentSlotId);
                    if (forwardedInternalBinding) {
                        slotForwardedInternalBindings[childSlotId].set(offset, forwardedInternalBinding);
                    }
                    break;
                }
            }

            const childBindings = new Map();
            for (let childSlotId = 0; childSlotId < childOpenSlots; childSlotId++) {
                const selectedBinding = slotSelectedBindings[childSlotId];
                if (!selectedBinding) {
                    continue;
                }

                const finalizedBinding = cloneBindingDescriptor(selectedBinding, childSlotId);
                if (!finalizedBinding) {
                    continue;
                }

                if (slotStaticTargets[childSlotId]) {
                    finalizedBinding.forwarded = cloneBindingMap(slotForwardedInternalBindings[childSlotId]);
                }

                childBindings.set(childSlotId, finalizedBinding);
            }

            await expandConnector({
                descriptor: childDescriptor,
                parentPath: `${fullPath}:${dimId}`,
                incomingBindings: childBindings,
                boundDescriptor: null,
                isRoot: false
            });

            openSlotId += childOpenSlotsInParent;
        }
    }

    await expandConnector({
        descriptor: draftConnectorDescriptor,
        parentPath: '',
        incomingBindings: new Map(),
        boundDescriptor: null,
        isRoot: true
    });

    return staticRiPositionOptions;
}

function scheduleConnectorStaticRiTreeRefresh(immediate = false) {
    if (connectorStaticRiTreeRefreshTimer) {
        clearTimeout(connectorStaticRiTreeRefreshTimer);
    }

    const runRefresh = () => {
        connectorStaticRiTreeRefreshTimer = null;
        void refreshConnectorStaticRiTreePositions();
    };

    if (immediate) {
        runRefresh();
        return;
    }

    connectorStaticRiTreeRefreshTimer = setTimeout(runRefresh, connectorStaticRiTreeRefreshDelayMs);
}

async function refreshConnectorStaticRiTreePositions() {
    const token = connectorStaticRiTreeRefreshToken + 1;
    connectorStaticRiTreeRefreshToken = token;
    setConnectorStaticRiTreeStatus('Loading static RI positions...', false);

    try {
        const options = await buildConnectorStaticRiPositionOptions();
        if (connectorStaticRiTreeRefreshToken !== token) {
            return;
        }

        connectorStaticRiPositionOptions = options;
        refreshConnectorStaticRiPositionInputs();
        setConnectorStaticRiTreeStatus(
            `${connectorStaticRiPositionOptions.length} DFS position(s) available (0..${Math.max(0, connectorStaticRiPositionOptions.length - 1)}).`,
            false
        );
    } catch (error) {
        if (connectorStaticRiTreeRefreshToken !== token) {
            return;
        }

        connectorStaticRiPositionOptions = [];
        refreshConnectorStaticRiPositionInputs();
        setConnectorStaticRiTreeStatus(
            error?.message || 'Failed to resolve static RI positions.',
            true
        );
    }

    renderConnectorPreviewBody();
    syncConnectorFormAvailability();
}

function setBindingPointStatus(dimensionElement, message) {
    const statusElement = dimensionElement.querySelector('.connector-binding-status');
    if (!statusElement) {
        return;
    }

    statusElement.textContent = message;
}

function clearDimensionBindings(dimensionElement) {
    const bindingsContainer = dimensionElement.querySelector('.connector-bindings');
    if (!bindingsContainer) {
        return false;
    }

    if (!bindingsContainer.querySelector('.connector-binding')) {
        return false;
    }

    bindingsContainer.innerHTML = '';
    return true;
}

function updateDimensionBindingAvailability(dimensionElement, { clearBindingsWhenScalar = false } = {}) {
    const compositeInput = dimensionElement.querySelector('.connector-dimension-composite');
    const compositeName = compositeInput ? compositeInput.value.trim() : '';
    const hasComposite = compositeName.length > 0;

    const addBindingButton = dimensionElement.querySelector('.connector-binding-add');
    if (addBindingButton) {
        setControlAvailability(
            addBindingButton,
            hasComposite,
            'Fill composite connector name first.'
        );
    }

    let clearedBindings = false;
    if (!hasComposite) {
        if (clearBindingsWhenScalar) {
            clearedBindings = clearDimensionBindings(dimensionElement);
        }

        const slotSelects = dimensionElement.querySelectorAll('.connector-binding-slot');
        slotSelects.forEach((slotSelect) => {
            const preferredSlot = slotSelect.dataset.preferredSlot || slotSelect.value;
            setBindingSlotSelectOptions(slotSelect, [], preferredSlot, 'Enter composite name first');
            updateBindingTargetAvailability(slotSelect.closest('.connector-binding'));
        });

        setBindingPointStatus(
            dimensionElement,
            'Scalar dimension cannot define bindings. Set composite name to enable bindings.'
        );
    }

    if (hasComposite) {
        dimensionElement
            .querySelectorAll('.connector-bindings > .connector-binding')
            .forEach((bindingElement) => updateBindingTargetAvailability(bindingElement));
    }

    return { hasComposite, clearedBindings };
}

function setBindingSlotSelectOptions(slotSelect, options, selectedSlot = '', emptyLabel = 'Select binding point') {
    const normalizedSelectedSlot = typeof selectedSlot === 'string' ? selectedSlot.trim() : '';

    slotSelect.innerHTML = '';

    const placeholderOption = document.createElement('option');
    placeholderOption.value = '';
    placeholderOption.textContent = emptyLabel;
    slotSelect.appendChild(placeholderOption);

    let hasSelectedOption = false;
    options.forEach(({ slotId, label }) => {
        const option = document.createElement('option');
        option.value = String(slotId);
        option.textContent = `${slotId} - ${label}`;
        slotSelect.appendChild(option);

        if (option.value === normalizedSelectedSlot) {
            hasSelectedOption = true;
        }
    });

    if (normalizedSelectedSlot && !hasSelectedOption) {
        const unresolvedOption = document.createElement('option');
        unresolvedOption.value = normalizedSelectedSlot;
        unresolvedOption.textContent = `${normalizedSelectedSlot} - unresolved`;
        slotSelect.appendChild(unresolvedOption);
        hasSelectedOption = true;
    }

    slotSelect.value = hasSelectedOption ? normalizedSelectedSlot : '';
    const shouldDisableSlot = options.length === 0 && !hasSelectedOption;
    setControlAvailability(slotSelect, !shouldDisableSlot, emptyLabel);
}

function scheduleBindingPointRefresh(dimensionElement, immediate = false) {
    const previousTimeout = connectorBindingRefreshTimers.get(dimensionElement);
    if (previousTimeout) {
        clearTimeout(previousTimeout);
    }

    const runRefresh = () => {
        connectorBindingRefreshTimers.delete(dimensionElement);
        void refreshDimensionBindingPoints(dimensionElement);
    };

    if (immediate) {
        runRefresh();
        return;
    }

    const timeoutHandle = setTimeout(runRefresh, 250);
    connectorBindingRefreshTimers.set(dimensionElement, timeoutHandle);
}

async function refreshDimensionBindingPoints(dimensionElement) {
    const token = (connectorBindingRefreshTokens.get(dimensionElement) || 0) + 1;
    connectorBindingRefreshTokens.set(dimensionElement, token);

    const { hasComposite } = updateDimensionBindingAvailability(dimensionElement, {
        clearBindingsWhenScalar: true
    });

    const slotSelects = Array.from(dimensionElement.querySelectorAll('.connector-binding-slot'));
    if (!hasComposite) {
        updateConnectorPreview();
        return;
    }

    if (slotSelects.length === 0) {
        setBindingPointStatus(dimensionElement, 'Binding points are loaded from selected composite.');
        return;
    }

    const compositeInput = dimensionElement.querySelector('.connector-dimension-composite');
    const compositeName = compositeInput ? compositeInput.value.trim() : '';

    slotSelects.forEach((slotSelect) => {
        const preferredSlot = slotSelect.dataset.preferredSlot || slotSelect.value;
        setBindingSlotSelectOptions(slotSelect, [], preferredSlot, 'Loading binding points...');
    });
    setBindingPointStatus(dimensionElement, `Loading binding points from '${compositeName}'...`);

    try {
        const options = await fetchBindingPointOptions(compositeName);
        if (connectorBindingRefreshTokens.get(dimensionElement) !== token) {
            return;
        }

        slotSelects.forEach((slotSelect) => {
            const preferredSlot = slotSelect.dataset.preferredSlot || slotSelect.value;
            setBindingSlotSelectOptions(
                slotSelect,
                options,
                preferredSlot,
                options.length === 0 ? 'No open binding points' : 'Select binding point'
            );
            delete slotSelect.dataset.preferredSlot;
        });

        if (options.length === 0) {
            setBindingPointStatus(dimensionElement, `No open binding points exported by '${compositeName}'.`);
        } else {
            setBindingPointStatus(dimensionElement, `${options.length} binding point(s) loaded from '${compositeName}'.`);
        }
    } catch (error) {
        if (connectorBindingRefreshTokens.get(dimensionElement) !== token) {
            return;
        }

        slotSelects.forEach((slotSelect) => {
            const preferredSlot = slotSelect.dataset.preferredSlot || slotSelect.value;
            setBindingSlotSelectOptions(slotSelect, [], preferredSlot, 'Failed to load binding points');
        });
        setBindingPointStatus(dimensionElement, error?.message || 'Failed to load binding points.');
    }

    updateConnectorPreview();
}

function parseConnectorDimensionsFromForm() {
    const dimensions = [];
    document.querySelectorAll('.connector-dimension').forEach((dimensionElement) => {
        const compositeInput = dimensionElement.querySelector('.connector-dimension-composite');
        const compositeName = compositeInput ? compositeInput.value.trim() : '';
        const bindings = {};
        dimensionElement.querySelectorAll('.connector-bindings > .connector-binding').forEach((bindingElement) => {
            const slotInput = bindingElement.querySelector('.connector-binding-slot');
            const targetInput = bindingElement.querySelector('.connector-binding-target');

            const slot = slotInput ? slotInput.value.trim() : '';
            if (!slot) {
                return;
            }

            const target = targetInput ? targetInput.value.trim() : '';
            if (!target) {
                return;
            }
            bindings[slot] = target;
        });

        const transformations = [];
        dimensionElement.querySelectorAll('.connector-transformations > .connector-transformation').forEach((transformationElement) => {
            const transformationNameInput = transformationElement.querySelector('.connector-transformation-name');
            const transformationArgsInput = transformationElement.querySelector('.connector-transformation-args');
            const transformationName = transformationNameInput ? transformationNameInput.value.trim() : '';
            if (!transformationName) {
                return;
            }

            const transformationArgs = parseIntList(transformationArgsInput ? transformationArgsInput.value : '');
            transformations.push({ name: transformationName, args: transformationArgs });
        });
        dimensions.push({
            composite: compositeName,
            bindings: compositeName ? bindings : {},
            transformations
        });
    });
    return dimensions;
}

function parseConnectorStaticRiFromForm() {
    const rowStates = [];
    const positionsMap = new Map();
    const allowedPositions = new Set();
    let maxAllowedPosition = 0;

    connectorStaticRiPositionOptions.forEach(({ position }) => {
        if (!Number.isInteger(position) || position < 0) {
            return;
        }
        allowedPositions.add(position);
        if (position > maxAllowedPosition) {
            maxAllowedPosition = position;
        }
    });

    document.querySelectorAll('#connectorStaticRiContainer > .connector-static-ri').forEach((entryElement, index) => {
        const rowIndex = index + 1;
        const positionInput = entryElement.querySelector('.connector-static-ri-position');
        const startPointInput = entryElement.querySelector('.connector-static-ri-start-point');
        const transformShiftInput = entryElement.querySelector('.connector-static-ri-transform-shift');
        setConnectorStaticRiEntryValidation(entryElement, '');

        const positionRaw = typeof positionInput?.value === 'string' ? positionInput.value.trim() : '';
        const startPointRaw = typeof startPointInput?.value === 'string' ? startPointInput.value.trim() : '';
        const transformShiftRaw = typeof transformShiftInput?.value === 'string' ? transformShiftInput.value.trim() : '';
        const hasAnyFieldValue = positionRaw.length > 0 || startPointRaw.length > 0 || transformShiftRaw.length > 0;

        const position = parseUint32Input(positionRaw);
        const startPoint = parseUint32Input(startPointRaw);
        const transformShift = parseUint32Input(transformShiftRaw);

        const rowIssues = [];
        if (!hasAnyFieldValue) {
            rowIssues.push('Empty static RI row. Fill all fields or remove the row.');
        }

        const missingFields = [];
        if (positionRaw.length === 0) {
            missingFields.push('position');
        }
        if (startPointRaw.length === 0) {
            missingFields.push('start point');
        }
        if (transformShiftRaw.length === 0) {
            missingFields.push('transform shift');
        }
        if (missingFields.length > 0 && hasAnyFieldValue) {
            rowIssues.push(`Missing ${missingFields.join(', ')}.`);
        }

        const invalidFields = [];
        if (positionRaw.length > 0 && position === null) {
            invalidFields.push('position');
        }
        if (startPointRaw.length > 0 && startPoint === null) {
            invalidFields.push('start point');
        }
        if (transformShiftRaw.length > 0 && transformShift === null) {
            invalidFields.push('transform shift');
        }
        if (invalidFields.length > 0) {
            rowIssues.push(`Invalid ${invalidFields.join(', ')} (must be uint32).`);
        }

        if (position !== null && allowedPositions.size > 0 && !allowedPositions.has(position)) {
            rowIssues.push(
                `Position ${position} is out of range for this connector (allowed 0..${maxAllowedPosition}).`
            );
        }

        const rowState = {
            entryElement,
            rowIndex,
            position,
            startPoint,
            transformShift,
            rowIssues
        };
        rowStates.push(rowState);

        if (position !== null) {
            if (!positionsMap.has(position)) {
                positionsMap.set(position, []);
            }
            positionsMap.get(position).push(rowState);
        }
    });

    for (const [position, entries] of positionsMap.entries()) {
        if (entries.length <= 1) {
            continue;
        }

        const duplicateMessage = `Duplicate position ${position}. Keep only one entry per position.`;
        entries.forEach((entry) => {
            entry.rowIssues.push(duplicateMessage);
        });
    }

    const staticRiEntries = [];
    const validationErrors = [];
    rowStates.forEach((rowState) => {
        const rowMessage = rowState.rowIssues.join(' ');
        setConnectorStaticRiEntryValidation(rowState.entryElement, rowMessage);
        if (rowMessage.length > 0) {
            validationErrors.push(`Row ${rowState.rowIndex}: ${rowMessage}`);
            return;
        }

        staticRiEntries.push([rowState.position, {
            start_point: rowState.startPoint,
            transformation_shift: rowState.transformShift
        }]);
    });

    connectorStaticRiValidationErrors = validationErrors;
    setConnectorStaticRiValidationStatus(connectorStaticRiValidationErrors);

    staticRiEntries.sort((lhs, rhs) => lhs[0] - rhs[0]);

    const staticRi = {};
    staticRiEntries.forEach(([position, runningInstance]) => {
        staticRi[String(position)] = runningInstance;
    });

    return staticRi;
}

function renumberConnectorDimensions() {
    const legends = document.querySelectorAll('.connector-dimension .dimension-fieldset > legend');
    legends.forEach((legend, index) => {
        legend.textContent = `Dimension ${index + 1}`;
    });
}

function addConnectorTransformation(addButton, transformation = {}) {
    const fieldset = addButton.closest('.dimension-fieldset');
    const container = fieldset ? fieldset.querySelector('.connector-transformations') : null;
    if (!container) {
        return;
    }

    const transformationElement = document.createElement('div');
    transformationElement.className = 'connector-transformation';

    const row = document.createElement('div');
    row.className = 'transformation-row';

    const nameColumn = document.createElement('div');
    nameColumn.className = 'transformation-col';
    const nameLabel = document.createElement('label');
    nameLabel.textContent = 'Transformation name';
    const nameInput = document.createElement('input');
    nameInput.type = 'text';
    nameInput.className = 'connector-transformation-name';
    nameInput.placeholder = 'add';
    nameInput.value = typeof transformation.name === 'string' ? transformation.name : '';
    nameInput.addEventListener('input', () => {
        updateTransformationArgsAvailability(transformationElement);
        updateConnectorPreview();
    });
    nameColumn.appendChild(nameLabel);
    nameColumn.appendChild(nameInput);

    const argsColumn = document.createElement('div');
    argsColumn.className = 'transformation-col';
    const argsLabel = document.createElement('label');
    argsLabel.textContent = 'Args (comma-separated)';
    const argsInput = document.createElement('input');
    argsInput.type = 'text';
    argsInput.className = 'connector-transformation-args';
    argsInput.placeholder = '...';
    argsInput.value = Array.isArray(transformation.args) ? transformation.args.join(', ') : '';
    argsInput.addEventListener('input', updateConnectorPreview);
    argsColumn.appendChild(argsLabel);
    argsColumn.appendChild(argsInput);

    row.appendChild(nameColumn);
    row.appendChild(argsColumn);

    const removeButton = document.createElement('button');
    removeButton.type = 'button';
    removeButton.className = 'connector-transformation-remove';
    removeButton.textContent = 'Remove';
    removeButton.addEventListener('click', () => {
        transformationElement.remove();
        updateConnectorPreview();
    });
    row.appendChild(removeButton);

    transformationElement.appendChild(row);
    container.appendChild(transformationElement);
    updateTransformationArgsAvailability(transformationElement);
    updateConnectorPreview();
}

function addConnectorBinding(addButton, binding = {}) {
    const fieldset = addButton.closest('.dimension-fieldset');
    const container = fieldset ? fieldset.querySelector('.connector-bindings') : null;
    if (!container) {
        return;
    }
    const dimensionElement = addButton.closest('.connector-dimension');
    if (!dimensionElement) {
        return;
    }

    const compositeInput = dimensionElement.querySelector('.connector-dimension-composite');
    if (!compositeInput || !compositeInput.value.trim()) {
        updateDimensionBindingAvailability(dimensionElement, { clearBindingsWhenScalar: true });
        updateConnectorPreview();
        return;
    }

    const bindingElement = document.createElement('div');
    bindingElement.className = 'connector-binding';

    const row = document.createElement('div');
    row.className = 'binding-row';

    const slotColumn = document.createElement('div');
    slotColumn.className = 'binding-col';
    const slotLabel = document.createElement('label');
    slotLabel.textContent = 'Binding point id';
    const slotInput = document.createElement('select');
    slotInput.className = 'connector-binding-slot';
    const initialSlot = typeof binding.slot === 'string' ? binding.slot.trim() : '';
    if (initialSlot) {
        slotInput.dataset.preferredSlot = initialSlot;
    }
    setBindingSlotSelectOptions(slotInput, [], initialSlot, 'Enter composite name first');
    slotInput.addEventListener('change', () => {
        updateBindingTargetAvailability(bindingElement);
        updateConnectorPreview();
    });
    slotColumn.appendChild(slotLabel);
    slotColumn.appendChild(slotInput);

    const targetColumn = document.createElement('div');
    targetColumn.className = 'binding-col';
    const targetLabel = document.createElement('label');
    targetLabel.textContent = 'Composite connector';
    const targetInput = document.createElement('input');
    targetInput.type = 'text';
    targetInput.className = 'connector-binding-target';
    targetInput.placeholder = 'TIME';
    targetInput.value = typeof binding.target === 'string' ? binding.target : '';
    targetInput.addEventListener('input', updateConnectorPreview);
    targetColumn.appendChild(targetLabel);
    targetColumn.appendChild(targetInput);

    const removeButton = document.createElement('button');
    removeButton.type = 'button';
    removeButton.className = 'connector-binding-remove';
    removeButton.textContent = 'Remove';
    removeButton.addEventListener('click', () => {
        bindingElement.remove();
        updateConnectorPreview();
    });

    row.appendChild(slotColumn);
    row.appendChild(targetColumn);
    row.appendChild(removeButton);
    bindingElement.appendChild(row);

    container.appendChild(bindingElement);
    updateBindingTargetAvailability(bindingElement);
    scheduleBindingPointRefresh(dimensionElement, true);
    updateConnectorPreview();
}

function appendConnectorDimension(dimension = {}) {
    const container = document.getElementById('connectorDimensionsContainer');
    if (!container) {
        return;
    }

    const dimensionElement = document.createElement('div');
    dimensionElement.className = 'connector-dimension';

    const fieldset = document.createElement('fieldset');
    fieldset.className = 'dimension-fieldset';
    const legend = document.createElement('legend');
    fieldset.appendChild(legend);

    const removeDimensionButton = document.createElement('button');
    removeDimensionButton.type = 'button';
    removeDimensionButton.className = 'connector-dimension-remove';
    removeDimensionButton.textContent = 'Remove dimension';
    removeDimensionButton.addEventListener('click', () => {
        const pendingRefreshTimeout = connectorBindingRefreshTimers.get(dimensionElement);
        if (pendingRefreshTimeout) {
            clearTimeout(pendingRefreshTimeout);
            connectorBindingRefreshTimers.delete(dimensionElement);
        }
        connectorBindingRefreshTokens.delete(dimensionElement);

        dimensionElement.remove();
        renumberConnectorDimensions();
        updateConnectorPreview();
    });

    const compositeLabel = document.createElement('label');
    compositeLabel.textContent = 'Composite connector name';
    const compositeInput = document.createElement('input');
    compositeInput.type = 'text';
    compositeInput.className = 'connector-dimension-composite';
    compositeInput.placeholder = 'leave empty for scalar';
    compositeInput.value = typeof dimension.composite === 'string' ? dimension.composite : '';
    compositeInput.addEventListener('input', () => {
        const { clearedBindings } = updateDimensionBindingAvailability(dimensionElement, {
            clearBindingsWhenScalar: true
        });
        updateConnectorPreview();
        if (clearedBindings) {
            setBindingPointStatus(
                dimensionElement,
                'Scalar dimension cannot define bindings. Existing bindings were cleared.'
            );
        }
        scheduleBindingPointRefresh(dimensionElement);
    });
    const compositeRow = document.createElement('div');
    compositeRow.className = 'connector-dimension-composite-row';
    compositeRow.appendChild(compositeInput);

    const addBindingButton = document.createElement('button');
    addBindingButton.type = 'button';
    addBindingButton.className = 'connector-binding-add';
    addBindingButton.textContent = 'Add binding';
    addBindingButton.addEventListener('click', () => addConnectorBinding(addBindingButton));
    compositeRow.appendChild(removeDimensionButton);

    fieldset.appendChild(compositeLabel);
    fieldset.appendChild(compositeRow);

    const bindingsStatus = document.createElement('div');
    bindingsStatus.className = 'connector-binding-status muted-note';
    bindingsStatus.textContent = 'Set composite name to load binding points.';
    fieldset.appendChild(bindingsStatus);

    const bindingsContainer = document.createElement('div');
    bindingsContainer.className = 'connector-bindings';
    fieldset.appendChild(bindingsContainer);
    fieldset.appendChild(addBindingButton);

    const transformationsContainer = document.createElement('div');
    transformationsContainer.className = 'connector-transformations';
    fieldset.appendChild(transformationsContainer);

    const addTransformationButton = document.createElement('button');
    addTransformationButton.type = 'button';
    addTransformationButton.textContent = 'Add transformation';
    addTransformationButton.addEventListener('click', () => addConnectorTransformation(addTransformationButton));
    fieldset.appendChild(addTransformationButton);

    dimensionElement.appendChild(fieldset);
    container.appendChild(dimensionElement);

    updateDimensionBindingAvailability(dimensionElement, { clearBindingsWhenScalar: true });

    if (
        compositeInput.value.trim() &&
        dimension.bindings &&
        typeof dimension.bindings === 'object' &&
        !Array.isArray(dimension.bindings)
    ) {
        Object.entries(dimension.bindings).forEach(([slot, target]) => {
            addConnectorBinding(addBindingButton, {
                slot,
                target: typeof target === 'string' ? target : ''
            });
        });
    }

    if (Array.isArray(dimension.transformations)) {
        dimension.transformations.forEach((transformation) => {
            addConnectorTransformation(addTransformationButton, transformation);
        });
    }

    scheduleBindingPointRefresh(dimensionElement, true);
    renumberConnectorDimensions();
}

function appendConnectorStaticRi(staticRi = {}) {
    const container = document.getElementById('connectorStaticRiContainer');
    if (!container) {
        return;
    }

    const staticRiElement = document.createElement('div');
    staticRiElement.className = 'connector-static-ri';

    const row = document.createElement('div');
    row.className = 'binding-row';

    const positionColumn = document.createElement('div');
    positionColumn.className = 'binding-col';
    const positionLabel = document.createElement('label');
    positionLabel.textContent = 'Connector position';
    const positionInput = document.createElement('select');
    positionInput.className = 'connector-static-ri-position';
    const selectedPosition = staticRi.position !== undefined && staticRi.position !== null
        ? String(staticRi.position)
        : '';
    setConnectorStaticRiPositionSelectOptions(positionInput, selectedPosition);
    positionInput.addEventListener('change', updateConnectorPreview);
    positionColumn.appendChild(positionLabel);
    positionColumn.appendChild(positionInput);

    const startPointColumn = document.createElement('div');
    startPointColumn.className = 'binding-col';
    const startPointLabel = document.createElement('label');
    startPointLabel.textContent = 'Start point';
    const startPointInput = document.createElement('input');
    startPointInput.type = 'number';
    startPointInput.min = '0';
    startPointInput.step = '1';
    startPointInput.className = 'connector-static-ri-start-point';
    startPointInput.placeholder = '0';
    if (staticRi.start_point !== undefined && staticRi.start_point !== null) {
        startPointInput.value = String(staticRi.start_point);
    }
    startPointInput.addEventListener('input', updateConnectorPreview);
    startPointColumn.appendChild(startPointLabel);
    startPointColumn.appendChild(startPointInput);

    const transformShiftColumn = document.createElement('div');
    transformShiftColumn.className = 'binding-col';
    const transformShiftLabel = document.createElement('label');
    transformShiftLabel.textContent = 'Transform shift';
    const transformShiftInput = document.createElement('input');
    transformShiftInput.type = 'number';
    transformShiftInput.min = '0';
    transformShiftInput.step = '1';
    transformShiftInput.className = 'connector-static-ri-transform-shift';
    transformShiftInput.placeholder = '0';
    if (staticRi.transformation_shift !== undefined && staticRi.transformation_shift !== null) {
        transformShiftInput.value = String(staticRi.transformation_shift);
    }
    transformShiftInput.addEventListener('input', updateConnectorPreview);
    transformShiftColumn.appendChild(transformShiftLabel);
    transformShiftColumn.appendChild(transformShiftInput);

    const removeButton = document.createElement('button');
    removeButton.type = 'button';
    removeButton.className = 'connector-static-ri-remove';
    removeButton.textContent = 'Remove';
    removeButton.addEventListener('click', () => {
        staticRiElement.remove();
        updateConnectorPreview();
    });

    row.appendChild(positionColumn);
    row.appendChild(startPointColumn);
    row.appendChild(transformShiftColumn);
    row.appendChild(removeButton);

    const errorElement = document.createElement('div');
    errorElement.className = 'connector-static-ri-error helper-text';

    staticRiElement.appendChild(row);
    staticRiElement.appendChild(errorElement);
    container.appendChild(staticRiElement);
}

export function addConnectorDimension() {
    appendConnectorDimension();
    updateConnectorPreview();
}

export function clearConnectorDimensions() {
    const container = document.getElementById('connectorDimensionsContainer');
    if (!container) {
        return;
    }
    container.innerHTML = '';
    updateConnectorPreview();
}

export function addConnectorStaticRi() {
    const defaultStaticRi = buildDefaultConnectorStaticRi();
    if (!defaultStaticRi) {
        updateConnectorPreview();
        return;
    }

    appendConnectorStaticRi(defaultStaticRi);
    updateConnectorPreview();
}

export function clearConnectorStaticRi() {
    const container = document.getElementById('connectorStaticRiContainer');
    if (!container) {
        return;
    }

    container.innerHTML = '';
    updateConnectorPreview();
}

function constructStructuredConnector() {
    const name = document.getElementById('in_connectorName').value.trim();
    const dimensions = parseConnectorDimensionsFromForm();
    const condition_name = document.getElementById('in_connectorConditionName').value.trim();
    const condition_args = condition_name
        ? parseIntList(document.getElementById('in_connectorConditionArgs').value)
        : [];
    const static_ri = parseConnectorStaticRiFromForm();

    return JSON.stringify({ name, dimensions, condition_name, condition_args, static_ri }, null, 2);
}

function renderConnectorPreviewBody() {
    const json = constructStructuredConnector();
    const requestBodyElement = document.getElementById('POST_connectorRequestBody');
    if (requestBodyElement) {
        requestBodyElement.textContent = json;
    }
}

export function updateConnectorPreview() {
    renderConnectorPreviewBody();
    syncConnectorFormAvailability();
    scheduleConnectorStaticRiTreeRefresh();
}

export function populateStructuredConnector(jsonOrObject) {
    const data = typeof jsonOrObject === 'string' ? JSON.parse(jsonOrObject) : jsonOrObject;

    if (!data || typeof data !== 'object') {
        console.warn("Invalid connector data:", data);
        return;
    }

    document.getElementById('in_connectorName').value = data.name || '';

    clearConnectorDimensions();
    const dimensions = Array.isArray(data.dimensions) ? data.dimensions : [];
    dimensions.forEach((dimension) => appendConnectorDimension(dimension));

    clearConnectorStaticRi();
    if (data.static_ri && typeof data.static_ri === 'object' && !Array.isArray(data.static_ri)) {
        Object.entries(data.static_ri)
            .sort((lhs, rhs) => Number.parseInt(lhs[0], 10) - Number.parseInt(rhs[0], 10))
            .forEach(([position, runningInstance]) => {
                appendConnectorStaticRi({
                    position,
                    start_point: runningInstance?.start_point,
                    transformation_shift: runningInstance?.transformation_shift
                });
            });
    }

    document.getElementById('in_connectorConditionName').value = data.condition_name || '';
    document.getElementById('in_connectorConditionArgs').value = Array.isArray(data.condition_args)
        ? data.condition_args.join(', ')
        : '';

    updateConnectorPreview();
}

export async function sendStructuredConnector() {
    const requestBody = constructStructuredConnector();

    const responseCodeDiv = document.getElementById('connectorResponseCode');
    const responseBodyDiv = document.getElementById('connectorResponseBody');

    try {
        const res = await requestWithLogin(apiUrl('/connector'), {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: requestBody,
        });
        const text = await res.text();
        responseCodeDiv.textContent = res.status;
        responseBodyDiv.textContent = formatJSON(text);
        if (res.ok) {
            // Connector definitions can change, which may affect binding points.
            clearConnectorBindingCaches();
            scheduleConnectorStaticRiTreeRefresh(true);
        }
    } catch (error) {
        responseCodeDiv.textContent = 'Error';
        responseBodyDiv.textContent = error.message;
    }
}

export async function getConnector() {
    const name = document.getElementById('in_connectorName').value.trim();

    const responseCodeDiv = document.getElementById('connectorResponseCode');
    const responseBodyDiv = document.getElementById('connectorResponseBody');

    if (!name) {
        alert("Connector name is required.");
        return;
    }

    const url = apiUrl(`/connector/${name}`);
    try {
        const res = await fetch(url);
        const text = await res.text();
        responseCodeDiv.textContent = res.status;
        responseBodyDiv.textContent = formatJSON(text);
        populateStructuredConnector(JSON.parse(text));
    } catch (error) {
        responseCodeDiv.textContent = 'Error';
        responseBodyDiv.textContent = error.message;
    }
}

// --------------------------------------------------------------------------
// Version
// --------------------------------------------------------------------------
export async function fetchVersionInfo() {
    const versionDiv = document.getElementById('versionInfo');
    try {
        const res = await fetch(apiUrl('/version'));
        const data = await res.json();
        versionDiv.textContent = `Version ${data.version} (Built: ${data.build_timestamp})`;
    } catch (err) {
        versionDiv.textContent = "⚠️ Failed to fetch version info";
        console.error("Version fetch failed:", err);
    }
}

// --------------------------------------------------------------------------
// Explore
// --------------------------------------------------------------------------
let currentAccountPage = 0;
const accountPageSize = 10;
let accountCursorState = {
    connectors: null,
    transformations: null,
    conditions: null
};
let accountNextCursorState = {
    connectors: null,
    transformations: null,
    conditions: null
};
let accountHasMore = false;
const accountCursorHistory = [];

let currentFormatPage = 0;
const formatPageSize = 10;
let formatAfterCursor = null;
let formatNextAfterCursor = null;
let formatHasMore = false;
const formatCursorHistory = [];
let lastAccountAddressInputValue = '';
let lastFormatHashInputValue = '';

let eventsFeedAbortController = null;
let eventsFeedReader = null;
let eventsFeedLastSeq = null;
let eventsFeedItems = [];
let eventsHistoryNextBeforeCursor = null;
const EVENTS_FEED_MAX_ITEMS = 50;

function escapeHtml(value) {
    return String(value ?? '')
        .replace(/&/g, '&amp;')
        .replace(/</g, '&lt;')
        .replace(/>/g, '&gt;')
        .replace(/"/g, '&quot;')
        .replace(/'/g, '&#39;');
}

function cloneAccountCursorState(state) {
    return {
        connectors: state.connectors,
        transformations: state.transformations,
        conditions: state.conditions
    };
}

function normalizeCursorToken(value) {
    if (typeof value !== 'string') {
        return null;
    }

    const trimmed = value.trim();
    return trimmed.length > 0 ? trimmed : null;
}

function findLastOwnedResourceEntry(entries) {
    if (!Array.isArray(entries)) {
        return null;
    }

    for (let i = entries.length - 1; i >= 0; i -= 1) {
        const normalized = normalizeCursorToken(entries[i]);
        if (normalized) {
            return normalized;
        }
    }

    return null;
}

function resolveNextAccountCursor(currentCursor, nextAfter, entries) {
    const explicitCursor = normalizeCursorToken(nextAfter);
    if (explicitCursor) {
        return explicitCursor;
    }

    const terminalCursor = findLastOwnedResourceEntry(entries);
    if (terminalCursor) {
        return terminalCursor;
    }

    return normalizeCursorToken(currentCursor);
}

function normalizeCursorObject(cursor) {
    if (!cursor || typeof cursor !== 'object') {
        return { hasMore: false, nextAfter: null };
    }

    return {
        hasMore: Boolean(cursor.has_more),
        nextAfter: normalizeCursorToken(cursor.next_after)
    };
}

function resetAccountPaginationState() {
    currentAccountPage = 0;
    accountCursorState = {
        connectors: null,
        transformations: null,
        conditions: null
    };
    accountNextCursorState = cloneAccountCursorState(accountCursorState);
    accountHasMore = false;
    accountCursorHistory.length = 0;
}

function resetFormatPaginationState() {
    currentFormatPage = 0;
    formatAfterCursor = null;
    formatNextAfterCursor = null;
    formatHasMore = false;
    formatCursorHistory.length = 0;
}

function renderOwnedResourceEntries(entries) {
    if (!Array.isArray(entries) || entries.length === 0) {
        return '(none)';
    }

    return entries
        .map((entry) => {
            if (typeof entry !== 'string') {
                return '';
            }
            return `<div class="account-item"><code>${escapeHtml(entry)}</code></div>`;
        })
        .join('');
}

function updateAccountPaginationControls() {
    const pageLabel = document.getElementById('accountPageLabel');
    const prevButton = document.getElementById('btn_prevAccountPage');
    const nextButton = document.getElementById('btn_nextAccountPage');
    if (!pageLabel || !prevButton || !nextButton) {
        return;
    }

    if (currentAccountPage < 0) {
        currentAccountPage = 0;
    }

    pageLabel.textContent = accountHasMore
        ? `Cursor page ${currentAccountPage + 1} (more)`
        : `Cursor page ${currentAccountPage + 1}`;
    prevButton.disabled = accountCursorHistory.length === 0;
    nextButton.disabled = accountHasMore === false;
}

function updateFormatPaginationControls() {
    const pageLabel = document.getElementById('formatPageLabel');
    const prevButton = document.getElementById('btn_prevFormatPage');
    const nextButton = document.getElementById('btn_nextFormatPage');
    if (!pageLabel || !prevButton || !nextButton) {
        return;
    }

    if (currentFormatPage < 0) {
        currentFormatPage = 0;
    }

    pageLabel.textContent = formatHasMore
        ? `Cursor page ${currentFormatPage + 1} (more)`
        : `Cursor page ${currentFormatPage + 1}`;
    prevButton.disabled = formatCursorHistory.length === 0;
    nextButton.disabled = formatHasMore === false;
}

function updateAccountFetchAvailability() {
    const addressInput = document.getElementById('accountAddressInput');
    const fetchButton = document.getElementById('btn_fetchAccountData');
    if (!addressInput || !fetchButton) {
        return;
    }

    ensureDisabledFieldTooltip();
    const hasAddress = addressInput.value.trim().length > 0;
    setControlAvailability(fetchButton, hasAddress, 'Fill account address first.');
}

function updateFormatFetchAvailability() {
    const hashInput = document.getElementById('formatHashInput');
    const fetchButton = document.getElementById('btn_fetchFormatData');
    if (!hashInput || !fetchButton) {
        return;
    }

    ensureDisabledFieldTooltip();
    const hasHash = hashInput.value.trim().length > 0;
    setControlAvailability(fetchButton, hasHash, 'Fill format hash first.');
}

function updateHistoryPaginationControls() {
    const nextButton = document.getElementById('btn_nextHistoryPage');
    if (nextButton) {
        nextButton.disabled = !eventsHistoryNextBeforeCursor;
    }
}

function resetHistoryPagination() {
    eventsHistoryNextBeforeCursor = null;
    updateHistoryPaginationControls();
}

function initializeAccountControls() {
    const addressInput = document.getElementById('accountAddressInput');
    if (addressInput) {
        lastAccountAddressInputValue = addressInput.value.trim();
        addressInput.addEventListener('input', () => {
            const normalizedAddress = addressInput.value.trim();
            const addressChanged = normalizedAddress !== lastAccountAddressInputValue;
            updateAccountFetchAvailability();
            if (addressChanged) {
                resetAccountPaginationState();
                updateAccountPaginationControls();
            }
            lastAccountAddressInputValue = normalizedAddress;
        });
    }

    const hashInput = document.getElementById('formatHashInput');
    if (hashInput) {
        lastFormatHashInputValue = hashInput.value.trim();
        hashInput.addEventListener('input', () => {
            const normalizedHash = hashInput.value.trim();
            const hashChanged = normalizedHash !== lastFormatHashInputValue;
            updateFormatFetchAvailability();
            if (hashChanged) {
                resetFormatPaginationState();
                updateFormatPaginationControls();
            }
            lastFormatHashInputValue = normalizedHash;
        });
    }

    updateAccountFetchAvailability();
    updateAccountPaginationControls();
    updateFormatFetchAvailability();
    updateFormatPaginationControls();
}

function initializeEventsHistoryControls() {
    for (const id of ['eventsHistoryLimitInput', 'eventsHistoryBeforeInput', 'eventsHistoryTypeInput']) {
        const input = document.getElementById(id);
        if (input) {
            input.addEventListener('input', resetHistoryPagination);
        }
    }
    updateHistoryPaginationControls();
}

function initializeEntityActionControls() {
    updateExecuteActionAvailability();
    syncTransformationFormAvailability();
    syncConditionFormAvailability();
    syncConnectorFormAvailability();
}

function initializeSimpleFormControls() {
    initializeEntityActionControls();
    initializeAccountControls();
    initializeEventsHistoryControls();
}

function scheduleSimpleFormInitialization() {
    if (typeof document === 'undefined') {
        return;
    }

    if (document.readyState === 'loading') {
        document.addEventListener('DOMContentLoaded', initializeSimpleFormControls, { once: true });
        return;
    }

    initializeSimpleFormControls();
}

export function nextPage()
{
    if (!accountHasMore) {
        return;
    }

    accountCursorHistory.push(cloneAccountCursorState(accountCursorState));
    accountCursorState = cloneAccountCursorState(accountNextCursorState);
    currentAccountPage += 1;
    fetchAccountResourcesPage(false);
}

export function prevPage()
{
    if (accountCursorHistory.length === 0) {
        return;
    }

    accountCursorState = accountCursorHistory.pop();
    if (currentAccountPage > 0) {
        currentAccountPage -= 1;
    }
    fetchAccountResourcesPage(false);
}

export function nextFormatPage()
{
    if (!formatHasMore || !formatNextAfterCursor) {
        return;
    }

    formatCursorHistory.push(formatAfterCursor);
    formatAfterCursor = formatNextAfterCursor;
    currentFormatPage += 1;
    fetchFormatResourcesPage(false);
}

export function prevFormatPage()
{
    if (formatCursorHistory.length === 0) {
        return;
    }

    formatAfterCursor = formatCursorHistory.pop();
    if (currentFormatPage > 0) {
        currentFormatPage -= 1;
    }
    fetchFormatResourcesPage(false);
}

async function fetchAccountResourcesPage(resetPagination) {
    const address = document.getElementById('accountAddressInput').value.trim();
    const transformationsDiv = document.getElementById('accountTransformationsList');
    const conditionsDiv = document.getElementById('accountConditionsList');
    const connectorsDiv = document.getElementById('accountConnectorsList');
    const prevButton = document.getElementById('btn_prevAccountPage');
    const nextButton = document.getElementById('btn_nextAccountPage');

    transformationsDiv.textContent = 'Loading...';
    conditionsDiv.textContent = 'Loading...';
    connectorsDiv.textContent = 'Loading...';
    updateAccountFetchAvailability();
    if (prevButton) {
        prevButton.disabled = true;
    }
    if (nextButton) {
        nextButton.disabled = true;
    }

    if (resetPagination) {
        resetAccountPaginationState();
    }

    if (!address) {
        alert("Address is required.");
        transformationsDiv.textContent = '';
        conditionsDiv.textContent = '';
        connectorsDiv.textContent = '';
        resetAccountPaginationState();
        updateAccountPaginationControls();
        return;
    }

    try {
        const queryParams = new URLSearchParams({
            limit: String(accountPageSize)
        });
        if (accountCursorState.connectors) {
            queryParams.set('after_connectors', accountCursorState.connectors);
        }
        if (accountCursorState.transformations) {
            queryParams.set('after_transformations', accountCursorState.transformations);
        }
        if (accountCursorState.conditions) {
            queryParams.set('after_conditions', accountCursorState.conditions);
        }

        const res = await fetch(apiUrl(`/account/${encodeURIComponent(address)}?${queryParams.toString()}`), {
            method: 'GET',
            headers: { 'Content-Type': 'application/json' }
        });

        const data = await res.json();
        if (!res.ok) {
            throw new Error(typeof data?.message === 'string' ? data.message : `HTTP ${res.status}`);
        }

        transformationsDiv.innerHTML = renderOwnedResourceEntries(data.owned_transformations);
        conditionsDiv.innerHTML = renderOwnedResourceEntries(data.owned_conditions);
        connectorsDiv.innerHTML = renderOwnedResourceEntries(data.owned_connectors);

        const connectorsCursor = normalizeCursorObject(data.cursor_connectors);
        const transformationsCursor = normalizeCursorObject(data.cursor_transformations);
        const conditionsCursor = normalizeCursorObject(data.cursor_conditions);

        accountNextCursorState = {
            connectors: resolveNextAccountCursor(
                accountCursorState.connectors,
                connectorsCursor.nextAfter,
                data.owned_connectors
            ),
            transformations: resolveNextAccountCursor(
                accountCursorState.transformations,
                transformationsCursor.nextAfter,
                data.owned_transformations
            ),
            conditions: resolveNextAccountCursor(
                accountCursorState.conditions,
                conditionsCursor.nextAfter,
                data.owned_conditions
            )
        };
        accountHasMore = connectorsCursor.hasMore || transformationsCursor.hasMore || conditionsCursor.hasMore;

        updateAccountPaginationControls();

    } catch (err) {
        const message = err instanceof Error && err.message
            ? err.message
            : 'Failed to fetch account data';
        transformationsDiv.textContent = `❌ ${message}`;
        conditionsDiv.textContent = '';
        connectorsDiv.textContent = '';
        accountHasMore = false;
        accountNextCursorState = cloneAccountCursorState(accountCursorState);
        updateAccountPaginationControls();
    }
}

export async function fetchAccountResources() {
    return fetchAccountResourcesPage(true);
}

async function fetchFormatResourcesPage(resetPagination) {
    const hashInput = document.getElementById('formatHashInput');
    const formatHash = hashInput ? hashInput.value.trim() : '';
    const scalarsDiv = document.getElementById('formatScalarsList');
    const connectorsDiv = document.getElementById('formatConnectorsList');
    const prevButton = document.getElementById('btn_prevFormatPage');
    const nextButton = document.getElementById('btn_nextFormatPage');

    if (!scalarsDiv || !connectorsDiv) {
        return;
    }

    scalarsDiv.textContent = 'Loading...';
    connectorsDiv.textContent = 'Loading...';
    updateFormatFetchAvailability();
    if (prevButton) {
        prevButton.disabled = true;
    }
    if (nextButton) {
        nextButton.disabled = true;
    }

    if (resetPagination) {
        resetFormatPaginationState();
    }

    if (!formatHash) {
        alert("Format hash is required.");
        scalarsDiv.textContent = '';
        connectorsDiv.textContent = '';
        resetFormatPaginationState();
        updateFormatPaginationControls();
        return;
    }

    try {
        const queryParams = new URLSearchParams({
            limit: String(formatPageSize)
        });
        if (formatAfterCursor) {
            queryParams.set('after', formatAfterCursor);
        }

        const res = await fetch(apiUrl(`/format/${encodeURIComponent(formatHash)}?${queryParams.toString()}`), {
            method: 'GET',
            headers: { 'Content-Type': 'application/json' }
        });

        const data = await res.json();
        if (!res.ok) {
            throw new Error(typeof data?.message === 'string' ? data.message : `HTTP ${res.status}`);
        }

        scalarsDiv.innerHTML = Array.isArray(data.scalars) && data.scalars.length
            ? data.scalars.map((scalar) => `<div class="account-item"><code>${escapeHtml(scalar)}</code></div>`).join('')
            : '(none)';

        connectorsDiv.innerHTML = Array.isArray(data.connectors) && data.connectors.length
            ? data.connectors.map((connectorName) => {
                const name = typeof connectorName === 'string' && connectorName.trim().length > 0
                    ? connectorName
                    : '(unnamed)';
                return `<div class="account-item"><strong><code>${escapeHtml(name)}</code></strong></div>`;
            }).join('')
            : '(none)';

        const formatCursor = normalizeCursorObject(data.cursor);

        formatHasMore = formatCursor.hasMore;
        formatNextAfterCursor = formatCursor.nextAfter;

        updateFormatPaginationControls();

    } catch (err) {
        const message = err instanceof Error && err.message
            ? err.message
            : 'Failed to fetch format data';
        scalarsDiv.textContent = `❌ ${message}`;
        connectorsDiv.textContent = '';
        formatHasMore = false;
        formatNextAfterCursor = formatAfterCursor;
        updateFormatPaginationControls();
    }
}

export async function fetchFormatResources() {
    return fetchFormatResourcesPage(true);
}

scheduleSimpleFormInitialization();

// --------------------------------------------------------------------------
// Events Feed (SSE)
// --------------------------------------------------------------------------

function setEventsFeedStatus(connected) {
    const dot = document.getElementById('eventsFeedStatusDot');
    const label = document.getElementById('eventsFeedStatusLabel');
    const connectBtn = document.getElementById('btn_connectEventsFeed');
    const disconnectBtn = document.getElementById('btn_disconnectEventsFeed');
    if (dot) dot.className = `status-dot ${connected ? 'status-dot--connected' : 'status-dot--disconnected'}`;
    if (label) label.textContent = connected ? 'Connected' : 'Disconnected';
    if (connectBtn) connectBtn.disabled = connected;
    if (disconnectBtn) disconnectBtn.disabled = !connected;
}

function updateEventsFeedSeqLabel(seq) {
    const label = document.getElementById('eventsFeedSeqLabel');
    if (label) label.textContent = seq != null ? `seq: ${seq}` : 'seq: —';
}

function renderEventsFeedItem(delta) {
    const date = delta.created_at_ms
        ? new Date(delta.created_at_ms).toLocaleString(undefined, {
              day: '2-digit', month: '2-digit', year: 'numeric',
              hour: '2-digit', minute: '2-digit'
          })
        : '?';
    const feedId = escapeHtml(delta.feed_id ?? '—');
    const eventType = escapeHtml(delta.event_type ?? '—');
    const status = delta.status ? escapeHtml(delta.status) : '';
    const cursor = delta.stream_seq != null
        ? `seq ${delta.stream_seq}`
        : delta.history_cursor != null
            ? `cursor ${delta.history_cursor}`
            : 'seq ?';

    const payload = delta.payload && typeof delta.payload === 'object' ? delta.payload : {};
    const payloadRows = [
        payload.name  != null ? `<div class="events-feed-payload-row"><span class="events-feed-payload-label">name</span><span class="events-feed-payload-value">${escapeHtml(String(payload.name))}</span></div>` : '',
        payload.type  != null ? `<div class="events-feed-payload-row"><span class="events-feed-payload-label">type</span><span class="events-feed-payload-value">${escapeHtml(String(payload.type))}</span></div>` : '',
        payload.owner != null ? `<div class="events-feed-payload-row"><span class="events-feed-payload-label">owner</span><span class="events-feed-payload-value">${escapeHtml(String(payload.owner))}</span></div>` : '',
    ].filter(Boolean).join('');

    return `<div class="events-feed-item">
        <div class="events-feed-item-header">
            <span class="events-feed-item-type">${eventType}</span>
            <span class="events-feed-item-time">${escapeHtml(date)}</span>
        </div>
        ${payloadRows ? `<div class="events-feed-item-payload">${payloadRows}</div>` : ''}
        <div class="events-feed-item-meta">
            <span class="events-feed-item-seq">${escapeHtml(cursor)}</span>
            ${status ? `<span class="events-feed-item-status">${status}</span>` : ''}
            <span class="events-feed-item-feed">${feedId}</span>
        </div>
    </div>`;
}

function renderEventsFeedItems(items) {
    return Array.isArray(items) && items.length > 0
        ? items.map(renderEventsFeedItem).join('')
        : '(none)';
}

function decodeQueryField(value) {
    try {
        return decodeURIComponent(value);
    } catch (_) {
        return value;
    }
}

function prependEventsFeedItem(delta) {
    eventsFeedItems.unshift(delta);
    if (eventsFeedItems.length > EVENTS_FEED_MAX_ITEMS) {
        eventsFeedItems.length = EVENTS_FEED_MAX_ITEMS;
    }
    const listDiv = document.getElementById('eventsFeedList');
    if (listDiv) listDiv.innerHTML = renderEventsFeedItems(eventsFeedItems);
}

export async function fetchHistoryEvents() {
    const limitInput = document.getElementById('eventsHistoryLimitInput');
    const beforeInput = document.getElementById('eventsHistoryBeforeInput');
    const typeInput = document.getElementById('eventsHistoryTypeInput');
    const listDiv = document.getElementById('eventsHistoryList');
    const limitRaw = limitInput ? limitInput.value.trim() : '';

    if (!/^\d+$/.test(limitRaw)) {
        alert('History limit must be a positive integer.');
        return;
    }

    const limit = Number.parseInt(limitRaw, 10);
    if (limit < 1 || limit > 256) {
        alert('History limit must be between 1 and 256.');
        return;
    }

    const params = [`limit=${limit}`];
    const before = beforeInput ? decodeQueryField(beforeInput.value.trim()) : '';
    const type = typeInput ? decodeQueryField(typeInput.value.trim()) : '';
    if (before) params.push(`before=${before}`);
    if (type) params.push(`type=${type}`);

    resetHistoryPagination();
    if (listDiv) listDiv.textContent = 'Loading...';

    try {
        const res = await fetch(apiUrl(`/feed?${params.join('&')}`));
        const data = await res.json();
        if (!res.ok) {
            throw new Error(typeof data?.message === 'string' ? data.message : `HTTP ${res.status}`);
        }

        eventsHistoryNextBeforeCursor = data?.cursor?.has_more
            ? normalizeCursorToken(data?.cursor?.next_before)
            : null;
        if (listDiv) listDiv.innerHTML = renderEventsFeedItems(data?.items);
    } catch (err) {
        const message = err instanceof Error && err.message ? err.message : 'Failed to fetch history';
        if (listDiv) listDiv.textContent = `❌ ${message}`;
    } finally {
        updateHistoryPaginationControls();
    }
}

export function nextHistoryPage() {
    if (!eventsHistoryNextBeforeCursor) {
        return;
    }

    const beforeInput = document.getElementById('eventsHistoryBeforeInput');
    if (beforeInput) {
        beforeInput.value = eventsHistoryNextBeforeCursor;
    }
    fetchHistoryEvents();
}

async function* parseSseStream(reader) {
    const decoder = new TextDecoder();
    let buffer = '';
    let frame = { id: null, event: 'message', data: [] };

    while (true) {
        const { done, value } = await reader.read();
        if (done) break;
        buffer += decoder.decode(value, { stream: true });

        const parts = buffer.split('\n');
        buffer = parts.pop();

        for (const line of parts) {
            if (line === '') {
                if (frame.data.length > 0 || frame.event !== 'message') {
                    yield { id: frame.id, event: frame.event, data: frame.data.join('\n') };
                }
                frame = { id: null, event: 'message', data: [] };
            } else if (line.startsWith(':')) {
                yield { comment: line.slice(1).trimStart() };
            } else if (line.startsWith('id:')) {
                frame.id = line.slice(3).trimStart();
            } else if (line.startsWith('event:')) {
                frame.event = line.slice(6).trimStart();
            } else if (line.startsWith('data:')) {
                frame.data.push(line.slice(5).trimStart());
            }
        }
    }
}

export async function connectEventsFeed() {
    if (eventsFeedAbortController) return;

    const sinceSeqInput = document.getElementById('eventsSinceSeqInput');
    const raw = sinceSeqInput ? sinceSeqInput.value.trim() : '';
    const sinceSeq = raw !== '' ? parseInt(raw, 10) : eventsFeedLastSeq;

    const params = new URLSearchParams();
    if (sinceSeq != null && !isNaN(sinceSeq)) {
        params.set('since_seq', String(sinceSeq));
    }

    const qs = params.toString();
    const url = apiUrl(`/feed/stream${qs ? '?' + qs : ''}`);

    const listDiv = document.getElementById('eventsFeedList');
    if (listDiv && eventsFeedItems.length === 0) listDiv.textContent = 'Connecting...';

    const abortController = new AbortController();
    eventsFeedAbortController = abortController;
    setEventsFeedStatus(true);

    try {
        const response = await fetch(url, { signal: abortController.signal });
        if (!response.ok) throw new Error(`HTTP ${response.status}`);
        if (!response.body) throw new Error('No response body');

        const reader = response.body.getReader();
        eventsFeedReader = reader;
        for await (const frame of parseSseStream(reader)) {
            if (abortController.signal.aborted) break;
            if (frame.comment !== undefined) continue;
            if (frame.event === 'stream_meta') {
                try {
                    const meta = JSON.parse(frame.data);
                    if (meta.last_seq != null) {
                        eventsFeedLastSeq = meta.last_seq;
                        updateEventsFeedSeqLabel(eventsFeedLastSeq);
                    }
                } catch (_) {}
                continue;
            }
            try {
                const delta = JSON.parse(frame.data);
                if (delta.stream_seq != null) {
                    eventsFeedLastSeq = delta.stream_seq;
                    updateEventsFeedSeqLabel(eventsFeedLastSeq);
                }
                prependEventsFeedItem(delta);
            } catch (_) {}
        }
    } catch (err) {
        if (!abortController.signal.aborted) {
            const errDiv = document.getElementById('eventsFeedList');
            if (errDiv && eventsFeedItems.length === 0) errDiv.textContent = `❌ ${err.message}`;
        }
    } finally {
        if (eventsFeedAbortController === abortController) {
            eventsFeedAbortController = null;
        }
        eventsFeedReader = null;
        setEventsFeedStatus(false);
    }
}

export function disconnectEventsFeed() {
    if (eventsFeedAbortController) {
        eventsFeedAbortController.abort();
        eventsFeedAbortController = null;
    }
    if (eventsFeedReader) {
        eventsFeedReader.cancel();
        eventsFeedReader = null;
    }
    setEventsFeedStatus(false);
}

// --------------------------------------------------------------------------
// Login
// --------------------------------------------------------------------------

// MetaMask login functionality
export async function loginWithMetaMask() 
{
    const loginStatusDiv = document.getElementById('loginStatus');

    if (typeof window.ethereum === 'undefined') {
        alert("MetaMask is not installed!");
        return false;
    }

    try {
        const [address] = await window.ethereum.request({ method: 'eth_requestAccounts' });
        const nonceRes = await fetch(apiUrl(`/nonce/${address}`));
        const { nonce } = await nonceRes.json();

        const message = `Login nonce: ${nonce}`;
        const signature = await window.ethereum.request({
            method: 'personal_sign',
            params: [message, address],
        });

        const authRes = await fetch(apiUrl('/auth'), {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ address, signature, message }),
        });

        const result = await authRes.json();

        if (result.access_token) {
            loginStatusDiv.innerHTML = `<p style="color: green;">✅ Authenticated as ${address}</p>`;
            setAccessToken(result.access_token);
            return true;
        }
        else {
            loginStatusDiv.innerHTML = `<p style="color: red;">❌ Authentication failed</p>`;
        }
    } catch (err) {
        console.error(err);
        loginStatusDiv.innerHTML = `<p style="color: red;">⚠️ Error: ${err.message}</p>`;
    }

    return false;
}
configureLoginHandler(loginWithMetaMask);
