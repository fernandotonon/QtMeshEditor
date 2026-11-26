import { useEffect, useMemo, useRef, useState } from 'react';
import { createPortal } from 'react-dom';
import styles from './App.module.css';
import CodePanel from './components/CodePanel';
import Section from './components/Section';
import VATDemo from './components/VATDemo';
import DemoVideo from './components/DemoVideo';
import useMeasuredHeight from './hooks/useMeasuredHeight';
import useQtmeshActionRef from './hooks/useQtmeshActionRef';
import {
  cloudBadgeMarkdown,
  cloudBadgeSteps,
  cloudBadgeUploadExample,
  footerLinks,
  hero,
  installOptions,
  links,
  pipelineExamples,
} from './data/content';

const showcases = [
  {
    title: 'Motion from a few words.',
    src: './media/motion.mp4',
    alt: 'QtMesh T2M text-to-motion generation demonstrated inside QtMesh Editor',
    caption: 'QtMesh T2M · local text-to-motion',
    body: 'Explore QtMesh T2M, an experimental lightweight motion model running locally. Generate a starting clip, then review the result in the editor.',
    post: 'https://www.linkedin.com/posts/fernando-tonon_generativeai-texttomotion-qtmesheditor-activity-7522353979931115521-8XJu',
  },
  {
    title: 'The details make the movement.',
    src: './media/hands.mp4',
    alt: 'Walk animation comparison with and without automatic hand pose handling',
    caption: 'NVIDIA Kimodo · hand poses & loops',
    body: 'Extend generated motion with prompt-aware hand presets and loop support. Small adjustments make animations easier to use in a game.',
    post: 'https://www.linkedin.com/posts/fernando-tonon_qtmesheditor-nvidia-kimodo-activity-7527110854723579904-Zq-X',
  },
];

const PLATFORM_BY_OS = {
  windows: 'Windows',
  macos: 'macOS',
  linux: 'Linux',
};

const FOCUSABLE_SELECTOR =
  'a[href], button:not([disabled]), textarea:not([disabled]), input:not([disabled]), select:not([disabled]), [tabindex]:not([tabindex="-1"])';

function detectVisitorOs() {
  if (typeof navigator === 'undefined') {
    return 'unknown';
  }

  const platform = (
    navigator.userAgentData?.platform ||
    navigator.platform ||
    ''
  ).toLowerCase();
  const userAgent = (navigator.userAgent || '').toLowerCase();
  const source = `${platform} ${userAgent}`;

  if (
    source.includes('android') ||
    source.includes('iphone') ||
    source.includes('ipad') ||
    source.includes('ipod') ||
    source.includes('mobile')
  ) {
    return 'unknown';
  }

  if (source.includes('win')) {
    return 'windows';
  }
  if (source.includes('mac') || source.includes('darwin')) {
    return 'macos';
  }
  if (source.includes('linux') || source.includes('x11')) {
    return 'linux';
  }
  return 'unknown';
}

/** Best-effort CPU arch from UA (Flatpak release bundle is x86_64-only). */
function detectVisitorArch() {
  if (typeof navigator === 'undefined') {
    return 'unknown';
  }
  const platform = (
    navigator.userAgentData?.platform ||
    navigator.platform ||
    ''
  ).toLowerCase();
  const userAgent = (navigator.userAgent || '').toLowerCase();
  const source = `${platform} ${userAgent}`;
  if (
    source.includes('aarch64') ||
    source.includes('arm64') ||
    /\barmv[678]\b/.test(source)
  ) {
    return 'arm64';
  }
  if (
    source.includes('x86_64') ||
    source.includes('amd64') ||
    source.includes('x86-64') ||
    source.includes('wow64')
  ) {
    return 'x86_64';
  }
  // Most desktop Linux UAs omit arch; default to x86_64 (matches Flatpak).
  return 'x86_64';
}

function getStoreLabel(method) {
  return method.split('/')[0].trim();
}

async function copyText(text) {
  if (typeof navigator !== 'undefined' && navigator.clipboard?.writeText) {
    await navigator.clipboard.writeText(text);
    return;
  }

  if (typeof document === 'undefined') {
    throw new Error('Clipboard is not available');
  }

  const input = document.createElement('textarea');
  input.value = text;
  input.setAttribute('readonly', '');
  input.style.position = 'absolute';
  input.style.left = '-9999px';
  document.body.appendChild(input);
  input.select();

  const copied = document.execCommand('copy');
  document.body.removeChild(input);

  if (!copied) {
    throw new Error('Copy failed');
  }
}

function App() {
  const [siteHeaderRef, siteHeaderHeight] = useMeasuredHeight();
  const [heroRef, heroHeight] = useMeasuredHeight();
  const [demoEnabled, setDemoEnabled] = useState(false);
  const [isInstallPortalOpen, setIsInstallPortalOpen] = useState(false);
  const [copyState, setCopyState] = useState('idle');
  const { actionRef: qtmeshActionRef, imageTag: qtmeshImageTag } =
    useQtmeshActionRef();
  const [activeCliTab, setActiveCliTab] = useState('scan');
  const portalDialogRef = useRef(null);
  const portalTriggerRef = useRef(null);
  const cliTabButtonRefs = useRef([]);
  const detectedOs = useMemo(detectVisitorOs, []);
  const detectedArch = useMemo(detectVisitorArch, []);
  const detectedPlatform = PLATFORM_BY_OS[detectedOs];
  // Flatpak is the cross-distro default on x86_64 (UA rarely names Fedora/RHEL).
  // The release .flatpak is amd64-only — steer ARM64 visitors to snap/RPM/deb.
  const recommendedInstall = useMemo(() => {
    if (!detectedPlatform) {
      return null;
    }
    if (detectedPlatform === 'Linux') {
      if (detectedArch === 'arm64') {
        return (
          installOptions.find(
            (item) => item.platform === 'Linux' && item.method === 'snap',
          ) ||
          installOptions.find((item) => item.platform === 'Fedora / RHEL') ||
          installOptions.find((item) => item.platform === 'Debian / Ubuntu') ||
          installOptions.find((item) => item.platform === 'Linux') ||
          null
        );
      }
      return (
        installOptions.find((item) => item.method === 'Flatpak') ||
        installOptions.find(
          (item) => item.platform === 'Linux' && item.method === 'snap',
        ) ||
        installOptions.find((item) => item.platform === 'Linux') ||
        null
      );
    }
    return (
      installOptions.find((item) => item.platform === detectedPlatform) || null
    );
  }, [detectedPlatform, detectedArch]);
  const recommendedStore = recommendedInstall
    ? getStoreLabel(recommendedInstall.method)
    : null;
  const portalInstallOptions = useMemo(() => {
    if (detectedPlatform === 'Linux') {
      return installOptions.filter(
        (item) =>
          item.platform === 'Linux' ||
          item.platform === 'Fedora / RHEL' ||
          item.platform === 'Debian / Ubuntu',
      );
    }
    if (recommendedInstall) {
      return [recommendedInstall];
    }
    return installOptions;
  }, [detectedPlatform, recommendedInstall]);
  const githubActionExample = useMemo(
    () =>
      pipelineExamples.githubAction
        .replace('__QTMESH_ACTION_REF__', qtmeshActionRef)
        .replace('__QTMESH_IMAGE_TAG__', qtmeshImageTag),
    [qtmeshActionRef, qtmeshImageTag],
  );

  const cliTabs = useMemo(
    () => [
      {
        id: 'scan',
        label: 'Scan',
        title: 'Scan repo assets',
        code: pipelineExamples.scan,
        language: 'scan',
      },
      {
        id: 'fix',
        label: 'Fix',
        title: 'Fix and optimize',
        code: pipelineExamples.fix,
        language: 'fix',
      },
      {
        id: 'convert',
        label: 'Convert',
        title: 'Convert formats',
        code: pipelineExamples.convert,
        language: 'convert',
      },
      {
        id: 'merge',
        label: 'Merge',
        title: 'Merge animation clips',
        code: pipelineExamples.merge,
        language: 'anim',
      },
      {
        id: 'turntable',
        label: 'Turntable',
        title: 'Render turntable PNG',
        code: pipelineExamples.turntable,
        language: 'turntable',
      },
      {
        id: 'isometric',
        label: 'Isometric',
        title: '8-direction sprite atlas',
        code: pipelineExamples.isometric,
        language: 'isometric',
      },
      {
        id: 'vat',
        label: 'VAT',
        title: 'Bake to vertex animation texture',
        code: pipelineExamples.vat,
        language: 'bash',
      },
      {
        id: 'docker',
        label: 'Docker',
        title: 'Docker workflow',
        code: pipelineExamples.docker,
        language: 'docker',
      },
      {
        id: 'gha',
        label: 'GitHub Actions',
        title: 'GitHub Actions workflow',
        code: githubActionExample,
        language: 'yaml',
      },
    ],
    [githubActionExample],
  );
  const activeCliExample =
    cliTabs.find((tab) => tab.id === activeCliTab) || cliTabs[0];
  const cliTabPanelId = 'cli-tab-panel';

  const cloudBadgePreview = useMemo(
    () => [
      {
        key: 'status',
        alt: 'QtMesh status badge',
        src: `${links.qtmeshCloudApi}/v1/u/u-28680b9c/p/qtmesheditor/badges/qtmesh-status.svg`,
      },
      {
        key: 'score',
        alt: 'QtMesh score badge',
        src: `${links.qtmeshCloudApi}/v1/u/u-28680b9c/p/qtmesheditor/badges/qtmesh-score.svg`,
      },
      {
        key: 'errors',
        alt: 'QtMesh errors badge',
        src: `${links.qtmeshCloudApi}/v1/u/u-28680b9c/p/qtmesheditor/badges/qtmesh-errors.svg`,
      },
      {
        key: 'warnings',
        alt: 'QtMesh warnings badge',
        src: `${links.qtmeshCloudApi}/v1/u/u-28680b9c/p/qtmesheditor/badges/qtmesh-warnings.svg`,
      },
    ],
    [],
  );

  useEffect(() => {
    if (!isInstallPortalOpen || typeof document === 'undefined') {
      return undefined;
    }

    const dialog = portalDialogRef.current;
    if (!dialog) {
      return undefined;
    }

    const collectFocusableElements = () =>
      Array.from(dialog.querySelectorAll(FOCUSABLE_SELECTOR)).filter(
        (element) =>
          element instanceof HTMLElement &&
          element.getAttribute('aria-hidden') !== 'true' &&
          element.tabIndex !== -1 &&
          element.getClientRects().length > 0,
      );

    const focusableElements = collectFocusableElements();
    (focusableElements[0] || dialog).focus();

    const onKeyDown = (event) => {
      if (event.key === 'Escape') {
        event.preventDefault();
        setIsInstallPortalOpen(false);
        return;
      }

      if (event.key !== 'Tab') {
        return;
      }

      const focusable = collectFocusableElements();
      if (focusable.length === 0) {
        event.preventDefault();
        dialog.focus();
        return;
      }

      const first = focusable[0];
      const last = focusable[focusable.length - 1];
      const activeElement = document.activeElement;

      if (!dialog.contains(activeElement)) {
        event.preventDefault();
        first.focus();
        return;
      }

      if (
        event.shiftKey &&
        (activeElement === first || activeElement === dialog)
      ) {
        event.preventDefault();
        last.focus();
        return;
      }

      if (!event.shiftKey && activeElement === last) {
        event.preventDefault();
        first.focus();
      }
    };

    const previousOverflow = document.body.style.overflow;
    document.body.style.overflow = 'hidden';
    window.addEventListener('keydown', onKeyDown);

    return () => {
      document.body.style.overflow = previousOverflow;
      window.removeEventListener('keydown', onKeyDown);
      portalTriggerRef.current?.focus();
    };
  }, [isInstallPortalOpen]);

  useEffect(() => {
    if (!isInstallPortalOpen) {
      setCopyState('idle');
    }
  }, [isInstallPortalOpen]);

  useEffect(() => {
    if (copyState === 'idle') {
      return undefined;
    }

    const timer = window.setTimeout(() => setCopyState('idle'), 2000);
    return () => window.clearTimeout(timer);
  }, [copyState]);

  // Deep-link to a section by URL fragment, e.g. `/#vat-demo`. The
  // browser's native fragment-scroll runs before React mounts the
  // sections, so the initial scroll lands on an empty page. After
  // mount, we re-resolve the hash and scroll to the matching element.
  // Also re-fires on hashchange so in-page navigation (clicking a
  // header anchor while staying on this page) works the same way.
  useEffect(() => {
    if (typeof window === 'undefined') {
      return undefined;
    }
    const scrollToHash = () => {
      const id = window.location.hash.replace(/^#/, '');
      if (!id) return;
      const target = document.getElementById(id);
      if (!target) return;
      // `behavior: 'auto'` for the initial scroll feels right —
      // smooth-scroll on a cold page is animated jank. Subsequent
      // in-page hashchanges use smooth.
      target.scrollIntoView({ behavior: 'auto', block: 'start' });
    };
    // requestAnimationFrame gives the DOM a tick to lay out before
    // we measure scroll positions; otherwise on slow CPU the target
    // may not have its final height yet.
    const raf = window.requestAnimationFrame(scrollToHash);
    const onHashChange = () => {
      const id = window.location.hash.replace(/^#/, '');
      if (!id) return;
      const target = document.getElementById(id);
      if (target) {
        target.scrollIntoView({ behavior: 'smooth', block: 'start' });
      }
    };
    window.addEventListener('hashchange', onHashChange);
    return () => {
      window.cancelAnimationFrame(raf);
      window.removeEventListener('hashchange', onHashChange);
    };
  }, []);

  async function handleCopyInstallCommand() {
    if (!recommendedInstall) {
      return;
    }

    try {
      await copyText(recommendedInstall.command);
      setCopyState('copied');
    } catch {
      setCopyState('error');
    }
  }

  function handleOpenInstallPortal(event) {
    portalTriggerRef.current = event.currentTarget;
    setIsInstallPortalOpen(true);
  }

  function handleCliTabKeyDown(event, index) {
    if (!['ArrowRight', 'ArrowLeft', 'Home', 'End'].includes(event.key)) {
      return;
    }

    event.preventDefault();
    const lastIndex = cliTabs.length - 1;
    let nextIndex = index;

    if (event.key === 'ArrowRight') {
      nextIndex = index === lastIndex ? 0 : index + 1;
    } else if (event.key === 'ArrowLeft') {
      nextIndex = index === 0 ? lastIndex : index - 1;
    } else if (event.key === 'Home') {
      nextIndex = 0;
    } else if (event.key === 'End') {
      nextIndex = lastIndex;
    }

    setActiveCliTab(cliTabs[nextIndex].id);
    const nextTab = cliTabButtonRefs.current[nextIndex];
    if (nextTab) nextTab.focus();
  }

  return (
    <>
      <div
        className={styles.page}
        style={{
          '--site-header-height': siteHeaderHeight
            ? `${siteHeaderHeight}px`
            : '90px',
        }}
      >
        <a className={styles.skipLink} href="#editor">
          Skip to content
        </a>
        <header ref={siteHeaderRef} className={styles.siteHeader}>
          <a
            className={styles.brand}
            href="#editor"
            aria-label="QtMesh Editor home"
          >
            <svg width="30" height="34" viewBox="0 0 30 34" aria-hidden="true">
              <path
                d="M15 1 29 9v16l-14 8L1 25V9Z"
                fill="none"
                stroke="currentColor"
                strokeWidth="2"
              />
              <path
                d="m1 9 14 8 14-8M15 17v16"
                fill="none"
                stroke="currentColor"
                strokeWidth="2"
              />
            </svg>
            <span>
              QtMesh <strong>Editor</strong>
            </span>
          </a>
          <nav className={styles.navigation} aria-label="Main navigation">
            <a href="#workflows">Workflows</a>
            <a href="#ecosystem">Ecosystem</a>
            <a href={links.docs}>Docs</a>
            <a href={links.github} target="_blank" rel="noreferrer">
              GitHub
            </a>
          </nav>
          <button
            className={styles.headerDownload}
            onClick={handleOpenInstallPortal}
          >
            Download
          </button>
        </header>
        <main id="editor" className={styles.main}>
          <header
            ref={heroRef}
            className={styles.hero}
            style={{
              '--panel-height': heroHeight ? `${heroHeight}px` : '100dvh',
            }}
          >
            <div className={styles.heroCopy}>
              <p className={styles.kicker}>Open-source 3D creation</p>
              <h1 className={styles.heroTitle}>
                Make it yours.
                <br />
                <span>Bring it to life.</span>
              </h1>
              <p className={styles.heroSubtitle}>
                Create, refine, and animate 3D assets. Hands-on editing,
                specialized AI models, and production tools in one open-source
                editor.
              </p>
              <div className={styles.ctaRow}>
                <button
                  className={`${styles.ctaButton} ${styles.ctaButtonPrimary}`}
                  onClick={handleOpenInstallPortal}
                >
                  Download QtMesh Editor
                </button>
                <a className={styles.textLink} href="#workflows">
                  Explore the workflows
                </a>
              </div>
              <p className={styles.platformLine}>
                Windows · macOS · Linux <span>MIT licensed</span>
              </p>
            </div>
            <figure className={styles.heroMedia}>
              <div className={styles.mediaToolbar}>
                <span>QTMESH EDITOR</span>
                <span>Performance capture</span>
              </div>
              <DemoVideo
                src="./media/capture.mp4"
                poster="./media/capture.webp"
                label="Face and body captured from one video, playing together inside QtMesh Editor"
              />
              <figcaption>
                <span>Face, hands, and body. One performance.</span>
                <a href="#mixamo">Explore animation</a>
              </figcaption>
            </figure>
          </header>
          <div
            className={styles.capabilityStrip}
            aria-label="Editor capabilities"
          >
            <span>Mesh & material editing</span>
            <span>Local AI workflows</span>
            <span>Rigging & animation</span>
            <span>CLI & automation</span>
          </div>

          <Section
            id="workflows"
            eyebrow="Inside the editor"
            title="Your asset. Every stage of the way."
            subtitle="Generate a starting point, work on the details, and take an animated asset into your game."
          >
            <div className={styles.workflowGrid}>
              <a href="#create" className={styles.workflowLink}>
                <span>Create & refine</span>
                <p>Shape, texture, and make it your own.</p>
              </a>
              <a href="#mixamo" className={styles.workflowLink}>
                <span>Rig & animate</span>
                <p>Build movement into your characters.</p>
              </a>
              <a href="#vat-demo" className={styles.workflowLink}>
                <span>Optimize & ship</span>
                <p>Prepare animation for your runtime.</p>
              </a>
            </div>
          </Section>

          <Section
            id="create"
            eyebrow="Create & refine"
            title="A starting point is just the beginning."
            subtitle="Combine AI-assisted generation with mesh editing, materials, and painting tools. Inspect the result and keep control of the details."
          >
            <figure className={styles.wideMedia}>
              <img
                src="./media/shape-guides.webp"
                alt="A knight reference image alongside 3D characters generated with different body shape guides and guide strengths"
                loading="lazy"
                width="2990"
                height="1672"
              />
              <figcaption>
                TRELLIS.2 shape guidance · steer the build and pose with your
                own base mesh ·{' '}
                <a
                  href="https://github.com/fernandotonon/QtMeshEditor/releases/tag/3.68.0"
                  target="_blank"
                  rel="noreferrer"
                >
                  Workflow details
                </a>
              </figcaption>
            </figure>
            <div className={styles.featureNotes}>
              <p>
                <strong>Generate locally</strong>Explore specialized models for
                3D creation and animation on supported hardware.
              </p>
              <p>
                <strong>Keep editing</strong>Adjust meshes, paint surfaces, and
                refine materials in the same workspace.
              </p>
              <p>
                <strong>Take it with you</strong>Convert assets for the next
                step in your pipeline.
              </p>
            </div>
          </Section>

          <Section
            id="mixamo"
            eyebrow="Rig & animate"
            title="Give your characters a little character."
            subtitle="Capture face and body from video, work with skeletons and skin weights, or explore motion generated from text."
          >
            <div className={styles.animationLayout}>
              <figure className={styles.wideMedia}>
                <DemoVideo
                  src="./media/skeleton.mp4"
                  poster="./media/skeleton.webp"
                  label="Bone weight and skeleton editing in QtMesh Editor"
                />
                <figcaption>Inspect skeletons and skin weights</figcaption>
              </figure>
              <div className={styles.editorialCopy}>
                <h3>From a shared rig to a full animation set.</h3>
                <p>
                  Bring together clips from Mixamo, Unreal, and other DCC tools.
                  Performance Capture can combine body movement and facial
                  expressions from one video into a single animation.
                </p>
                <a className={styles.textLink} href={`${links.docs}#cmd-anim`}>
                  Read the animation guide
                </a>
              </div>
            </div>
            <div className={styles.motionGrid}>
              {showcases.map((item) => (
                <article className={styles.motionStory} key={item.title}>
                  <figure className={styles.wideMedia}>
                    <DemoVideo
                      src={item.src}
                      poster={item.src.replace('.mp4', '.webp')}
                      label={item.alt}
                    />
                    <figcaption>{item.caption}</figcaption>
                  </figure>
                  <h3>{item.title}</h3>
                  <p>{item.body}</p>
                  <a
                    className={styles.textLink}
                    href={item.post}
                    target="_blank"
                    rel="noreferrer"
                  >
                    See the original demo
                  </a>
                </article>
              ))}
            </div>
          </Section>

          <Section
            id="vat-demo"
            eyebrow="Optimize & ship"
            title="Small animation data. Big crowds."
            subtitle="Bake motion into vertex or bone textures and play it back on the GPU. Compare the techniques with your own browser and hardware."
          >
            <figure className={styles.crowdMedia}>
              <DemoVideo
                src="./media/crowd.mp4"
                poster="./media/crowd.webp"
                label="Side-by-side VAT and bone texture animation of a 5,000-character crowd in Godot"
              />
              <figcaption>
                VAT and BTA · the same 5,000-character crowd ·{' '}
                <a
                  href="https://www.linkedin.com/posts/fernando-tonon_qtmesheditor-gamedevelopment-gpu-activity-7529748521894424576-AByI"
                  target="_blank"
                  rel="noreferrer"
                >
                  Demo details
                </a>
              </figcaption>
            </figure>
            <div className={styles.demoIntro}>
              <div>
                <h3>Try the live comparison</h3>
                <p>
                  Switch between VAT, skeletal animation, and BTA. Up to 5,000
                  instances, running in Godot.
                </p>
              </div>
              <button
                className={styles.ctaButton}
                onClick={() => setDemoEnabled(!demoEnabled)}
                aria-expanded={demoEnabled}
                aria-controls="live-demo"
              >
                {demoEnabled ? 'Close live demo' : 'Launch live demo'}
              </button>
            </div>
            <div id="live-demo">{demoEnabled && <VATDemo />}</div>
            <p className={styles.technicalNote}>
              BTA stores three RGBA texels per bone per frame. VAT stores
              animation per vertex. Performance depends on the mesh, scene, GPU,
              and runtime.
            </p>
          </Section>

          <Section
            id="ecosystem"
            eyebrow="The QtMesh ecosystem"
            title="Made here. Connected beyond the editor."
            subtitle="The editor, connected services, and games built with the tools share the same foundation."
          >
            <div className={styles.ecosystemGrid}>
              <article className={styles.ecosystemEditor}>
                <p className={styles.productLabel}>CREATE</p>
                <h3>QtMesh Editor</h3>
                <p>
                  Your desktop workspace for 3D assets, animation, and
                  automation.
                </p>
                <a className={styles.textLink} href="#install">
                  Get the editor
                </a>
              </article>
              <article>
                <p className={styles.productLabel}>CONNECT</p>
                <h3>QtMesh Cloud</h3>
                <p>
                  Keep asset quality reports, project history, and CI results
                  connected to your workflow.
                </p>
                <a
                  className={styles.textLink}
                  href={links.qtmeshCloud}
                  target="_blank"
                  rel="noreferrer"
                >
                  Explore QtMesh Cloud
                </a>
              </article>
              <article>
                <p className={styles.productLabel}>PLAY</p>
                <h3>QtMesh Games</h3>
                <p>
                  See assets created with QtMesh Editor in action, in playable
                  games.
                </p>
                <a
                  className={styles.textLink}
                  href="https://games.qtmesh.dev"
                  target="_blank"
                  rel="noreferrer"
                >
                  Discover the games
                </a>
              </article>
            </div>
            <div className={styles.gameShowcase}>
              <div>
                <p className={styles.kicker}>Made with QtMesh Editor</p>
                <h3>From the workspace to the game.</h3>
                <p>Explore the worlds and characters built using the editor.</p>
              </div>
              <a
                href="https://games.qtmesh.dev"
                target="_blank"
                rel="noreferrer"
              >
                <img
                  src="./media/scrapfall.jpg"
                  alt="Scrapfall game artwork"
                  loading="lazy"
                />
                <span>Scrapfall</span>
              </a>
              <a
                href="https://games.qtmesh.dev"
                target="_blank"
                rel="noreferrer"
              >
                <img
                  src="./media/ironfang.jpg"
                  alt="Ironfang game artwork"
                  loading="lazy"
                />
                <span>Ironfang</span>
              </a>
              <a
                href="https://games.qtmesh.dev"
                target="_blank"
                rel="noreferrer"
              >
                <img
                  src="./media/data-core-clash.jpg"
                  alt="Data Core Clash game artwork"
                  loading="lazy"
                />
                <span>Data Core Clash</span>
              </a>
            </div>
            <p className={styles.identityNote}>
              <strong>QtMesh ID</strong> Your account for connected QtMesh
              services.{' '}
              <a href={links.qtmeshCloud} target="_blank" rel="noreferrer">
                Access QtMesh Cloud
              </a>
            </p>
          </Section>

          <Section
            id="cli"
            eyebrow="Automate the repetitive work"
            title="The same tools, beyond the interface."
            subtitle="Scan, fix, convert, and merge with the CLI. Bring asset checks into Docker and GitHub Actions, or connect external tools through REST and MCP."
          >
            <div
              className={styles.cliTabs}
              role="tablist"
              aria-label="CLI and CI examples"
            >
              {cliTabs.map((tab, index) => {
                const isActive = activeCliExample.id === tab.id;
                const tabId = `cli-tab-${tab.id}`;
                return (
                  <button
                    key={tab.id}
                    id={tabId}
                    type="button"
                    role="tab"
                    aria-selected={isActive}
                    aria-controls={cliTabPanelId}
                    tabIndex={isActive ? 0 : -1}
                    className={`${styles.cliTabButton} ${isActive ? styles.cliTabButtonActive : ''}`}
                    onClick={() => setActiveCliTab(tab.id)}
                    onKeyDown={(event) => handleCliTabKeyDown(event, index)}
                    ref={(node) => {
                      cliTabButtonRefs.current[index] = node;
                    }}
                  >
                    {tab.label}
                  </button>
                );
              })}
            </div>

            <div
              className={styles.cliTabPanel}
              id={cliTabPanelId}
              role="tabpanel"
              aria-labelledby={`cli-tab-${activeCliExample.id}`}
            >
              <CodePanel
                title={activeCliExample.title}
                code={activeCliExample.code}
                label={activeCliExample.language}
              />
            </div>

            <div className={styles.cliLinks}>
              <a href={links.marketplace} target="_blank" rel="noreferrer">
                GitHub Action
              </a>
              <a href={links.docs} target="_blank" rel="noreferrer">
                CLI Documentation
              </a>
              <a href={links.releases} target="_blank" rel="noreferrer">
                Latest release binaries
              </a>
            </div>
          </Section>
          <Section
            id="qtmesh-cloud-badges"
            eyebrow="Connected quality reports"
            title="Keep an eye on what ships."
            subtitle="Send scan results to QtMesh Cloud and track asset quality across branches and commits."
          >
            <details className={styles.expandable}>
              <summary>Explore Cloud reporting and CI setup</summary>
              <div className={styles.expandableBody}>
                <div className={styles.badgeShowcase}>
                  {cloudBadgePreview.map((badge) => (
                    <article
                      key={badge.key}
                      className={styles.badgeShowcaseCard}
                    >
                      <img src={badge.src} alt={badge.alt} loading="lazy" />
                    </article>
                  ))}
                </div>

                <div className={styles.cloudStepGrid}>
                  {cloudBadgeSteps.map((item, index) => (
                    <article key={item.title} className={styles.cloudStepCard}>
                      <span className={styles.cloudStepIndex}>{index + 1}</span>
                      <h3>{item.title}</h3>
                      <p>{item.body}</p>
                    </article>
                  ))}
                </div>

                <details className={styles.expandable}>
                  <summary>Show full GitHub upload example</summary>
                  <div className={styles.expandableBody}>
                    <CodePanel
                      title="GitHub Actions: upload scan report"
                      code={cloudBadgeUploadExample}
                      label="yaml"
                    />
                  </div>
                </details>

                <details className={styles.expandable}>
                  <summary>Show README badge snippet</summary>
                  <pre className={styles.inlineCodeBlock}>
                    <code>{cloudBadgeMarkdown}</code>
                  </pre>
                </details>

                <div className={styles.cliLinks}>
                  <a href={links.qtmeshCloud} target="_blank" rel="noreferrer">
                    Open QtMesh Cloud
                  </a>
                  <a
                    href={`${links.qtmeshCloudApi}/v1/u/u-28680b9c/p/qtmesheditor/badges/qtmesh-status.svg`}
                    target="_blank"
                    rel="noreferrer"
                  >
                    View live badge example
                  </a>
                </div>
              </div>
            </details>
          </Section>
          <Section
            id="install"
            eyebrow="Start creating"
            title="Your next asset starts here."
            subtitle="Download QtMesh Editor for Windows, macOS, or Linux. Choose a release binary or your preferred package manager."
          >
            <div className={styles.installPortalEntryActions}>
              <button
                type="button"
                className={`${styles.ctaButton} ${styles.ctaButtonPrimary}`}
                onClick={handleOpenInstallPortal}
              >
                {hero.ctaPrimary}
              </button>
              <a href={links.releases} target="_blank" rel="noreferrer">
                Download latest release binaries
              </a>
            </div>

            <div className={styles.installGrid}>
              {installOptions.map((option) => (
                <article
                  key={`${option.platform}-${option.method}`}
                  className={styles.installMethodCard}
                >
                  <div className={styles.installMethodHeader}>
                    <h3>{option.platform}</h3>
                    <span>{option.method}</span>
                  </div>
                  <pre>
                    <code>{option.command}</code>
                  </pre>
                </article>
              ))}
            </div>
          </Section>
          <Section
            id="trust"
            eyebrow="Open source since 2012"
            title="Built in the open. Yours to explore."
            subtitle="Read the code, try the tools, report an issue, or help shape what comes next."
          >
            <div className={styles.openSourceLinks}>
              <a href={links.github} target="_blank" rel="noreferrer">
                Source code
              </a>
              <a href={links.allReleases} target="_blank" rel="noreferrer">
                Release history
              </a>
              <a href={links.issues} target="_blank" rel="noreferrer">
                Contribute & report issues
              </a>
              <a href={links.license} target="_blank" rel="noreferrer">
                MIT license
              </a>
            </div>
          </Section>
        </main>

        <footer className={styles.footer}>
          <div className={styles.footerInner}>
            <p>QtMesh Editor</p>
            <div className={styles.footerLinks}>
              {footerLinks.map((item) => (
                <a
                  key={item.label}
                  href={item.href}
                  target="_blank"
                  rel="noreferrer"
                >
                  {item.label}
                </a>
              ))}
            </div>
          </div>
        </footer>
      </div>

      {isInstallPortalOpen &&
        typeof document !== 'undefined' &&
        createPortal(
          <div
            className={styles.installPortalBackdrop}
            onClick={() => setIsInstallPortalOpen(false)}
            role="presentation"
          >
            <div
              className={styles.installPortalDialog}
              ref={portalDialogRef}
              tabIndex={-1}
              role="dialog"
              aria-modal="true"
              aria-labelledby="install-portal-title"
              onClick={(event) => event.stopPropagation()}
            >
              <div className={styles.installPortalHeader}>
                <div>
                  <p className={styles.installPortalEyebrow}>Install Portal</p>
                  <h2
                    id="install-portal-title"
                    className={styles.installPortalTitle}
                  >
                    {recommendedInstall
                      ? `Detected ${recommendedInstall.platform}: install via ${recommendedStore}`
                      : 'Could not detect your OS automatically'}
                  </h2>
                </div>
                <button
                  type="button"
                  className={styles.installPortalClose}
                  onClick={() => setIsInstallPortalOpen(false)}
                  aria-label="Close install portal"
                >
                  Close
                </button>
              </div>

              <div className={styles.installPortalGrid}>
                {portalInstallOptions.length > 0 ? (
                  portalInstallOptions.map((option) => {
                    const isRecommended =
                      recommendedInstall &&
                      option.platform === recommendedInstall.platform &&
                      option.method === recommendedInstall.method;
                    return (
                      <article
                        key={`${option.platform}-${option.method}`}
                        className={`${styles.installPortalCard}${
                          isRecommended
                            ? ` ${styles.installPortalCardRecommended}`
                            : ''
                        }`}
                      >
                        <div className={styles.installPortalCardTop}>
                          <h3>{option.platform}</h3>
                          <div className={styles.installPortalCardActions}>
                            <span>{option.method}</span>
                            {isRecommended ? (
                              <button
                                type="button"
                                className={styles.installPortalCopyButton}
                                onClick={handleCopyInstallCommand}
                              >
                                {copyState === 'copied'
                                  ? 'Copied'
                                  : copyState === 'error'
                                    ? 'Retry'
                                    : 'Copy'}
                              </button>
                            ) : null}
                          </div>
                        </div>
                        <pre>
                          <code>{option.command}</code>
                        </pre>
                      </article>
                    );
                  })
                ) : (
                  <p className={styles.installPortalFallback}>
                    OS detection is unavailable in this browser. Use the latest
                    release download links below.
                  </p>
                )}
              </div>

              <div className={styles.installPortalLinks}>
                <a href={links.releases} target="_blank" rel="noreferrer">
                  Download file from latest release
                </a>
                <a href={links.allReleases} target="_blank" rel="noreferrer">
                  Browse all releases
                </a>
              </div>
            </div>
          </div>,
          document.body,
        )}
    </>
  );
}

export default App;
