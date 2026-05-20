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
              {t.label}
            </button>
          );
        })}
      </div>

      <div
        role="tabpanel"
        id={`vat-panel-${tab.id}`}
        aria-labelledby={`vat-tab-${tab.id}`}
        className={styles.wrapper}
      >
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
