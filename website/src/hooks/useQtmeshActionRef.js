import { useEffect, useState } from 'react';

const QTMESH_RELEASES_LATEST_API = 'https://api.github.com/repos/fernandotonon/QtMeshEditor/releases/latest';
const QTMESH_ACTION_REF_FALLBACK = 'fernandotonon/QtMeshEditor@3.36.1';
const CACHE_KEY = 'qtmesh.actionRef.cache.v1';
const CACHE_TTL_MS = 6 * 60 * 60 * 1000;

let inFlightRequest = null;
let memoryCache = null;

function actionRefFromTag(tagName) {
  const tag = String(tagName || '').trim();
  if (!tag || !/^v?\d/.test(tag)) return QTMESH_ACTION_REF_FALLBACK;
  return `fernandotonon/QtMeshEditor@${tag}`;
}

/** Semver-ish action refs pin the ghcr image; everything else (e.g. @v1) tracks :latest. */
export function imageTagFromActionRef(ref) {
  const m = /^fernandotonon\/QtMeshEditor@(v?\d+\.\d+\.\d+)$/.exec(String(ref || ''));
  if (!m) return 'latest';
  return m[1].replace(/^v/, '');
}

function isValidCache(payload, now) {
  if (!payload || typeof payload !== 'object') return false;
  if (typeof payload.ref !== 'string' || !payload.ref.startsWith('fernandotonon/QtMeshEditor@')) return false;
  if (typeof payload.fetchedAt !== 'number') return false;
  if (now - payload.fetchedAt > CACHE_TTL_MS) return false;
  return true;
}

function readCache() {
  const now = Date.now();
  if (memoryCache && isValidCache(memoryCache, now)) {
    return memoryCache;
  }

  if (typeof window === 'undefined') {
    return null;
  }

  try {
    const stored = window.localStorage.getItem(CACHE_KEY);
    if (!stored) return null;
    const parsed = JSON.parse(stored);
    if (!isValidCache(parsed, now)) return null;
    memoryCache = parsed;
    return parsed;
  } catch (_error) {
    return null;
  }
}

function writeCache(ref) {
  const payload = { ref, fetchedAt: Date.now() };
  memoryCache = payload;
  if (typeof window === 'undefined') return;
  try {
    window.localStorage.setItem(CACHE_KEY, JSON.stringify(payload));
  } catch (_error) {
    // Ignore storage failures (private mode, quota, etc).
  }
}

async function fetchLatestActionRef() {
  if (inFlightRequest) {
    return inFlightRequest;
  }

  inFlightRequest = (async () => {
    const response = await fetch(QTMESH_RELEASES_LATEST_API, {
      headers: { Accept: 'application/vnd.github+json' },
    });
    if (!response.ok) {
      throw new Error(`GitHub API request failed with status ${response.status}`);
    }
    const payload = await response.json();
    const resolvedRef = actionRefFromTag(payload?.tag_name);
    writeCache(resolvedRef);
    return resolvedRef;
  })();

  try {
    return await inFlightRequest;
  } finally {
    inFlightRequest = null;
  }
}

export default function useQtmeshActionRef() {
  const cached = readCache();
  const [qtmeshActionRef, setQtmeshActionRef] = useState(cached?.ref || QTMESH_ACTION_REF_FALLBACK);

  useEffect(() => {
    const freshCache = readCache();
    if (freshCache?.ref) {
      setQtmeshActionRef(freshCache.ref);
      return undefined;
    }

    let cancelled = false;
    fetchLatestActionRef()
      .then((resolvedRef) => {
        if (!cancelled) setQtmeshActionRef(resolvedRef);
      })
      .catch(() => {
        // Keep fallback ref when GitHub API is unavailable.
      });

    return () => {
      cancelled = true;
    };
  }, []);

  return {
    actionRef: qtmeshActionRef,
    imageTag: imageTagFromActionRef(qtmeshActionRef),
  };
}
