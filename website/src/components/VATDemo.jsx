import { useState } from 'react';
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
        1000 instances of the same dancer, all driven by VAT in a single{' '}
        <code>MultiMeshInstance3D</code>. One batched draw call per
        surface for the entire crowd. Compare against the "1000× skeletal"
        tab to see the cost difference.
      </>
    ),
  },
  {
    id: 'perf_skeleton',
    label: '1000× skeletal',
    caption: (
      <>
        Same 1000 dancers driven by Godot's stock <code>SkinnedMeshRenderer</code>{' '}
        + per-instance <code>AnimationPlayer</code>. This is the typical
        path a Godot game ships with for NPC crowds; per-instance bone
        matrix uploads dominate. Compare FPS against the VAT tab — VAT
        should be substantially faster on every GPU.
      </>
    ),
  },
];

const QTMESH_REPO_URL =
  'https://github.com/fernandotonon/QtMeshEditor/tree/master/tools/vat-shaders';

export default function VATDemo() {
  const [tab, setTab] = useState(TABS[0]);

  // The Godot Web export ships a single 38 MB bundle with a Bootstrap
  // scene that reads `?scene=` from the URL and routes to the right
  // sub-scene. We change the iframe key so React fully remounts it
  // when the tab changes — Godot can't switch scenes via a query
  // string change to the SAME iframe document because the scene-load
  // happened in _ready() before the URL update could be observed.
  return (
    <div className={styles.demo}>
      <div className={styles.tabs} role="tablist" aria-label="VAT demo selector">
        {TABS.map((t) => (
          <button
            key={t.id}
            type="button"
            role="tab"
            aria-selected={tab.id === t.id}
            className={`${styles.tab} ${tab.id === t.id ? styles.tabActive : ''}`}
            onClick={() => setTab(t)}
          >
            {t.label}
          </button>
        ))}
      </div>

      <div className={styles.wrapper}>
        <iframe
          key={tab.id}
          src={`demo/index.html?scene=${tab.id}`}
          title={`QtMeshEditor VAT demo — ${tab.label}`}
          className={styles.frame}
          loading="lazy"
          allow="autoplay; fullscreen; cross-origin-isolated"
        />
      </div>

      <div className={styles.caption}>
        <p>{tab.caption}</p>
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
