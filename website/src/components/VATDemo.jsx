import { useRef, useState } from 'react';
import styles from './VATDemo.module.css';

const TABS = [
  {
    id: 'web',
    label: 'Showcase',
    caption: (
      <>
        One Rumba dancer driven by a baked Vertex Animation Texture — no
        skeleton at runtime, no per-frame CPU animation tick. Drag to
        orbit, scroll to zoom.
      </>
    ),
  },
  {
    id: 'perf_vat',
    label: '1000× VAT',
    caption: (
      <>
        The same dancer instanced, all driven by a vertex animation texture
        (VAT) in a single <code>MultiMeshInstance3D</code>: every vertex's
        position for every frame is baked into a texture, so the GPU only
        looks up positions. One batched draw call per surface for the whole
        crowd. Best for non-skeletal deformation and the cheapest shader.
      </>
    ),
  },
  {
    id: 'perf_skeleton',
    label: '1000× skeletal',
    caption: (
      <>
        The same dancers driven by Godot's stock skinned <code>MeshInstance3D</code>{' '}
        + a per-instance <code>AnimationPlayer</code> — the path a Godot game
        usually ships for NPC crowds. Every instance is its own draw call
        per surface and ticks its own skeleton on the CPU.
      </>
    ),
  },
  {
    id: 'perf_bta',
    label: '1000× bone texture',
    caption: (
      <>
        Bone texture animation (BTA): the clip is baked as one skinning
        matrix per bone per frame (70 bones × 72 frames ≈ 240 KB of float
        texture instead of a 5828-vertex position texture), and the vertex
        shader skins the shared bind-pose mesh on the GPU. Still one{' '}
        <code>MultiMeshInstance3D</code>; each instance picks its own clip,
        time offset, speed and crossfade through per-instance data. Bake it
        with <code>qtmesh vat &lt;file&gt; --mode bone-texture --target godot</code>.
      </>
    ),
  },
];

/* Instance counts offered on the perf tabs (forwarded as `&n=`). */
const COUNTS = [100, 400, 1000, 5000];

const BENCH_URL =
  'https://github.com/fernandotonon/QtMeshEditor/tree/master/tools/godot-vat-demo#perf-comparison';

const QTMESH_REPO_URL =
  'https://github.com/fernandotonon/QtMeshEditor/tree/master/tools/vat-shaders';

export default function VATDemo() {
  const [tab, setTab] = useState(TABS[0]);
  const [count, setCount] = useState(1000);
  const isPerf = tab.id.startsWith('perf_');
  const tabRefs = useRef([]);

  // Roving-focus arrow-key navigation per WAI-ARIA tab pattern:
  // Left/Right (and Home/End) move focus and activate the new tab.
  const onTabKeyDown = (event, index) => {
    let nextIndex = null;
    if (event.key === 'ArrowRight') {
      nextIndex = (index + 1) % TABS.length;
    } else if (event.key === 'ArrowLeft') {
      nextIndex = (index - 1 + TABS.length) % TABS.length;
    } else if (event.key === 'Home') {
      nextIndex = 0;
    } else if (event.key === 'End') {
      nextIndex = TABS.length - 1;
    } else {
      return;
    }
    event.preventDefault();
    setTab(TABS[nextIndex]);
    tabRefs.current[nextIndex]?.focus();
  };

  // The Godot Web export ships a single 38 MB bundle with a Bootstrap
  // scene that reads `?scene=` from the URL and routes to the right
  // sub-scene. We change the iframe key so React fully remounts it
  // when the tab changes — Godot can't switch scenes via a query
  // string change to the SAME iframe document because the scene-load
  // happened in _ready() before the URL update could be observed.
  return (
    <div className={styles.demo}>
      <div className={styles.tabs} role="tablist" aria-label="VAT demo selector">
        {TABS.map((t, i) => {
          const selected = tab.id === t.id;
          return (
            <button
              key={t.id}
              ref={(el) => { tabRefs.current[i] = el; }}
              type="button"
              role="tab"
              id={`vat-tab-${t.id}`}
              aria-selected={selected}
              aria-controls={`vat-panel-${t.id}`}
              tabIndex={selected ? 0 : -1}
              className={`${styles.tab} ${selected ? styles.tabActive : ''}`}
              onClick={() => setTab(t)}
              onKeyDown={(e) => onTabKeyDown(e, i)}
            >
              {t.id.startsWith('perf_') ? t.label.replace('1000', count.toLocaleString('en-US')) : t.label}
            </button>
          );
        })}
      </div>

      {isPerf && (
        <div className={styles.counts} role="group" aria-label="Instance count">
          <span className={styles.countsLabel}>Instances:</span>
          {COUNTS.map((n) => (
            <button
              key={n}
              type="button"
              aria-pressed={count === n}
              className={`${styles.count} ${count === n ? styles.countActive : ''}`}
              onClick={() => setCount(n)}
            >
              {n.toLocaleString('en-US')}
            </button>
          ))}
        </div>
      )}

      <div
        role="tabpanel"
        id={`vat-panel-${tab.id}`}
        aria-labelledby={`vat-tab-${tab.id}`}
        className={styles.wrapper}
      >
        <iframe
          key={`${tab.id}-${isPerf ? count : ''}`}
          src={`demo/index.html?scene=${tab.id}${isPerf ? `&n=${count}` : ''}`}
          title={`QtMeshEditor VAT demo — ${tab.label}`}
          className={styles.frame}
          loading="lazy"
          allow="autoplay; fullscreen; cross-origin-isolated"
        />
      </div>

      <div className={styles.caption}>
        <p>{tab.caption}</p>
        {isPerf && (
          <p className={styles.cliHint}>
            The overlay shows FPS, draw calls and the GPU texture / buffer memory Godot reports
            for the whole scene. Browser numbers depend heavily on your GPU and on WebGL;
            measured native numbers are in{' '}
            <a href={BENCH_URL} target="_blank" rel="noreferrer">tools/godot-vat-demo/README.md</a>.
          </p>
        )}
        <p className={styles.cliHint}>
          Bake locally with <code>qtmesh vat &lt;file&gt; --anim &lt;name&gt; -o &lt;dir&gt;</code>{' '}
          — drop-in shader templates for Godot, Unity, and Unreal ship at{' '}
          <a href={QTMESH_REPO_URL} target="_blank" rel="noreferrer">
            tools/vat-shaders/
          </a>
          .
        </p>
      </div>
    </div>
  );
}
