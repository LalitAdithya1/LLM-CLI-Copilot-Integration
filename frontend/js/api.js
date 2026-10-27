export const BACKEND_BASE_URL = 'http://127.0.0.1:8080';

export class ApiError extends Error {
  constructor(code, message, status = null) {
    super(message);
    this.name = 'ApiError';
    this.code = code;
    this.status = status;
  }
}

function isObject(value) {
  return value !== null && typeof value === 'object' && !Array.isArray(value);
}

function expectFields(payload, fields, endpointName) {
  const hasExactFields = isObject(payload)
    && Object.keys(payload).length === fields.length
    && fields.every((field) => Object.hasOwn(payload, field));

  if (!hasExactFields) {
    throw new ApiError(
      'INVALID_RESPONSE',
      `The backend returned an invalid ${endpointName} response.`,
    );
  }
}

function validateHealth(payload) {
  expectFields(payload, ['status'], 'health');
  if (payload.status !== 'ok') {
    throw new ApiError(
      'INVALID_RESPONSE',
      'The backend returned an invalid health response.',
    );
  }
  return payload;
}

function validateConfig(payload) {
  expectFields(
    payload,
    ['supportedExtensions', 'launchDirectory', 'activeOutputDirectory'],
    'configuration',
  );
  if (
    !Array.isArray(payload.supportedExtensions)
    || !payload.supportedExtensions.every((extension) => typeof extension === 'string')
    || typeof payload.launchDirectory !== 'string'
    || typeof payload.activeOutputDirectory !== 'string'
  ) {
    throw new ApiError(
      'INVALID_RESPONSE',
      'The backend returned an invalid configuration response.',
    );
  }
  return payload;
}

function validateGeneration(payload) {
  expectFields(payload, ['generatedCode', 'language'], 'generation');
  if (typeof payload.generatedCode !== 'string' || typeof payload.language !== 'string') {
    throw new ApiError(
      'INVALID_RESPONSE',
      'The backend returned an invalid generation response.',
    );
  }
  return payload;
}

export function createApiClient({
  baseUrl = BACKEND_BASE_URL,
  fetchImpl = globalThis.fetch,
} = {}) {
  const normalizedBaseUrl = String(baseUrl).replace(/\/+$/, '');

  async function requestJson(path, { method = 'GET', body } = {}) {
    if (typeof fetchImpl !== 'function') {
      throw new ApiError(
        'NETWORK_ERROR',
        'Fetch is not available in this environment.',
      );
    }

    const options = {
      method,
      headers: { Accept: 'application/json' },
    };

    if (body !== undefined) {
      options.headers['Content-Type'] = 'application/json';
      try {
        options.body = JSON.stringify(body);
      } catch {
        throw new ApiError('INVALID_REQUEST', 'Request data could not be serialized.');
      }
    }

    let response;
    try {
      response = await fetchImpl(`${normalizedBaseUrl}${path}`, options);
    } catch {
      throw new ApiError(
        'NETWORK_ERROR',
        'Unable to reach the CodeAgent backend. Check that the local server is running.',
      );
    }

    let responseText;
    try {
      responseText = await response.text();
    } catch {
      throw new ApiError(
        'NETWORK_ERROR',
        'The connection ended before the backend response could be read.',
        response.status,
      );
    }

    let payload;
    try {
      payload = JSON.parse(responseText);
    } catch {
      if (!response.ok) {
        throw new ApiError(
          'HTTP_ERROR',
          `The backend request failed with HTTP ${response.status}.`,
          response.status,
        );
      }
      throw new ApiError(
        'INVALID_RESPONSE',
        'The backend returned malformed JSON.',
        response.status,
      );
    }

    if (!response.ok) {
      const backendError = isObject(payload) && isObject(payload.error)
        ? payload.error
        : null;
      const code = typeof backendError?.code === 'string' && backendError.code.length > 0
        ? backendError.code
        : 'HTTP_ERROR';
      const message = typeof backendError?.message === 'string'
        && backendError.message.trim().length > 0
        ? backendError.message
        : `The backend request failed with HTTP ${response.status}.`;

      throw new ApiError(code, message, response.status);
    }

    return payload;
  }

  return Object.freeze({
    async getHealth() {
      return validateHealth(await requestJson('/api/health'));
    },

    async getConfig() {
      return validateConfig(await requestJson('/api/config'));
    },

    async generateCode(request = {}) {
      const fields = isObject(request) ? request : {};
      const payload = await requestJson('/api/generate', {
        method: 'POST',
        body: {
          instruction: fields.instruction,
          language: fields.language,
        },
      });
      return validateGeneration(payload);
    },
  });
}

const defaultClient = createApiClient();

export const getHealth = () => defaultClient.getHealth();
export const getConfig = () => defaultClient.getConfig();
export const generateCode = (request) => defaultClient.generateCode(request);
