import { useEffect, useMemo, useRef, useState } from 'react';
import { createPortal } from 'react-dom';
import styles from './App.module.css';
import ButtonLink from './components/ButtonLink';
import CodePanel from './components/CodePanel';
import FeatureCard from './components/FeatureCard';
import Section from './components/Section';
import VATDemo from './components/VATDemo';
import useQtmeshActionRef from './hooks/useQtmeshActionRef';
import {
  audienceCards,
  beforeAfter,
  cloudBadgeMarkdown,
  cloudBadgeSteps,
  cloudBadgeUploadExample,
  footerLinks,
  hero,
  highlightFeatures,
  installOptions,
  links,
  media,
  mixamoSteps,
  pipelineExamples,
  pipelineFlow,
  proofPoints,
  trustItems,
} from './data/content';

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

  const platform = (navigator.userAgentData?.platform || navigator.platform || '').toLowerCase();
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
  const [isInstallPortalOpen, setIsInstallPortalOpen] = useState(false);
  const [copyState, setCopyState] = useState('idle');
  const { actionRef: qtmeshActionRef, imageTag: qtmeshImageTag } = useQtmeshActionRef();
  const [activeCliTab, setActiveCliTab] = useState('scan');
  const portalDialogRef = useRef(null);
  const portalTriggerRef = useRef(null);
  const cliTabButtonRefs = useRef([]);
  const detectedOs = useMemo(detectVisitorOs, []);
  const detectedPlatform = PLATFORM_BY_OS[detectedOs];
  const recommendedInstall = useMemo(
    () => installOptions.find((item) => item.platform === detectedPlatform) || null,
    [detectedPlatform]
  );
  const recommendedStore = recommendedInstall ? getStoreLabel(recommendedInstall.method) : null;
  const githubActionExample = useMemo(
    () => pipelineExamples.githubAction
      .replace('__QTMESH_ACTION_REF__', qtmeshActionRef)
      .replace('__QTMESH_IMAGE_TAG__', qtmeshImageTag),
    [qtmeshActionRef, qtmeshImageTag]
  );

  const cliTabs = useMemo(
    () => [
      { id: 'scan', label: 'Scan', title: 'Scan repo assets', code: pipelineExamples.scan, language: 'scan' },
      { id: 'fix', label: 'Fix', title: 'Fix and optimize', code: pipelineExamples.fix, language: 'fix' },
      { id: 'convert', label: 'Convert', title: 'Convert formats', code: pipelineExamples.convert, language: 'convert' },
      { id: 'merge', label: 'Merge', title: 'Merge animation clips', code: pipelineExamples.merge, language: 'anim' },
      { id: 'turntable', label: 'Turntable', title: 'Render turntable PNG', code: pipelineExamples.turntable, language: 'turntable' },
      { id: 'docker', label: 'Docker', title: 'Docker workflow', code: pipelineExamples.docker, language: 'docker' },
      { id: 'gha', label: 'GitHub Actions', title: 'GitHub Actions workflow', code: githubActionExample, language: 'yaml' },
    ],
    [githubActionExample]
  );
  const activeCliExample = cliTabs.find((tab) => tab.id === activeCliTab) || cliTabs[0];
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
    []
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
          element.getClientRects().length > 0
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

      if (event.shiftKey && (activeElement === first || activeElement === dialog)) {
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
      <div className={styles.page}>
        <div className={styles.backdrop} aria-hidden="true" />

        <main className={styles.main}>
          <header className={`${styles.hero} reveal`}>
            <div className={styles.heroCopy}>
              <p className={styles.kicker}>QtMeshEditor | CI/CD for 3D Assets</p>
              <h1 className={styles.heroTitle}>{hero.title}</h1>
              <p className={styles.heroSubtitle}>{hero.subtitle}</p>

              <div className={styles.heroCtaPanel}>
                <div className={styles.ctaRow}>
                  <button
                    type="button"
                    className={`${styles.ctaButton} ${styles.ctaButtonPrimary}`}
                    onClick={handleOpenInstallPortal}
                  >
                    {hero.ctaPrimary}
                  </button>
                  <ButtonLink href={links.github} variant="secondary">
                    {hero.ctaSecondary}
                  </ButtonLink>
                </div>
                <p className={styles.ctaHint}>Install from winget, Homebrew, snap, Docker, or release binaries.</p>
              </div>

              <ul className={styles.proofRow} aria-label="Product proof points">
                {proofPoints.map((item) => (
                  <li key={item} className={styles.proofPill}>
                    {item}
                  </li>
                ))}
              </ul>
            </div>

            <figure className={styles.heroMediaCard}>
              <img className={styles.heroImage} src={media.pipelineCiCd.src} alt={media.pipelineCiCd.alt} />
              <figcaption className={styles.mediaCaption}>Scan → Validate → Upload → Badges</figcaption>
              <div className={styles.heroWorkflowBar} aria-label="Pipeline workflow overview">
                <span>Scan</span>
                <span>Validate</span>
                <span>Upload</span>
                <span>Badges</span>
              </div>
              <div className={styles.heroBadgeStrip}>
                {cloudBadgePreview.slice(0, 3).map((badge) => (
                  <img key={badge.key} src={badge.src} alt={badge.alt} loading="lazy" />
                ))}
              </div>
            </figure>
          </header>

          <Section
            id="quick-workflow"
            eyebrow="Quick Workflow"
            title="Understand the pipeline in one glance"
            subtitle="One strong flow: catch issues early, keep imports predictable, and publish visible quality signals."
          >
            <div className={styles.quickWorkflowLayout}>
              <figure className={`${styles.demoMain} reveal`}>
                <img className={styles.demoImage} src={media.skeletonPreview.src} alt={media.skeletonPreview.alt} loading="lazy" />
              </figure>

              <article className={styles.quickWorkflowPanel}>
                <h3>Commit-to-report workflow</h3>
                <ul className={styles.workflowList}>
                  {pipelineFlow.map((item) => (
                    <li key={item.title} className={styles.workflowListItem}>
                      <strong>{item.title}</strong>
                      <p>{item.body}</p>
                    </li>
                  ))}
                </ul>
              </article>
            </div>
          </Section>

          <Section
            id="qtmesh-cloud-badges"
            eyebrow="QtMesh Cloud"
            title="Track 3D asset quality in CI"
            subtitle="Get visibility, quality gates, and historical tracking for your 3D asset pipeline."
          >
            <p className={styles.sectionLeadCompact}>
              QtMesh Cloud turns each scan into a shareable quality signal for developers, technical artists, and production leads.
            </p>

            <div className={styles.badgeShowcase}>
              {cloudBadgePreview.map((badge) => (
                <article key={badge.key} className={styles.badgeShowcaseCard}>
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
                <CodePanel title="GitHub Actions: upload scan report" code={cloudBadgeUploadExample} label="yaml" />
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
              <a href={`${links.qtmeshCloudApi}/v1/u/u-28680b9c/p/qtmesheditor/badges/qtmesh-status.svg`} target="_blank" rel="noreferrer">
                View live badge example
              </a>
            </div>
          </Section>

          <Section
            id="before-after"
            eyebrow="Before vs After"
            title="From asset pipeline surprises to predictable delivery"
            subtitle="A small change in process creates a big difference in production reliability."
          >
            <div className={styles.beforeAfterGrid}>
              <article className={`${styles.outcomeCard} ${styles.outcomeBefore}`}>
                <h3>Before</h3>
                <ul>
                  {beforeAfter.before.map((item) => (
                    <li key={item}>
                      <span aria-hidden="true">✕</span>
                      <p>{item}</p>
                    </li>
                  ))}
                </ul>
              </article>

              <article className={`${styles.outcomeCard} ${styles.outcomeAfter}`}>
                <h3>After</h3>
                <ul>
                  {beforeAfter.after.map((item) => (
                    <li key={item}>
                      <span aria-hidden="true">✓</span>
                      <p>{item}</p>
                    </li>
                  ))}
                </ul>
              </article>
            </div>
          </Section>

          <Section
            id="who-for"
            eyebrow="Who It's For"
            title="Built for teams shipping 3D content continuously"
            subtitle="Focused workflows for game developers, technical artists, and studio CI/CD teams."
          >
            <div className={styles.audienceGrid}>
              {audienceCards.map((item) => (
                <FeatureCard key={item.title} title={item.title} body={item.body} tag={item.tag} />
              ))}
            </div>
          </Section>

          <Section
            id="mixamo"
            eyebrow="Animation Merge Workflow"
            title="Merge animation clips into one predictable output"
            subtitle="Combine Mixamo, Unreal, and DCC clips, then export a clean asset ready for engine import and CI."
          >
            <div className={styles.demoLayout}>
              <figure className={`${styles.demoMain} reveal`}>
                <img className={styles.demoImage} src={media.mergeDemo.src} alt={media.mergeDemo.alt} loading="lazy" />
              </figure>

              <ol className={styles.steps}>
                {mixamoSteps.map((step, index) => (
                  <li key={step.title} className={styles.stepItem}>
                    <span className={styles.stepIndex}>{index + 1}</span>
                    <div>
                      <h3 className={styles.stepTitle}>{step.title}</h3>
                      <p className={styles.stepBody}>{step.body}</p>
                    </div>
                  </li>
                ))}
              </ol>
            </div>

            <div className={styles.mixamoActions}>
              <ButtonLink href={`${links.docs}#cmd-anim`} variant="secondary">
                See animation workflow
              </ButtonLink>
            </div>
          </Section>

          <Section
            id="vat-demo"
            eyebrow="Live demo"
            title="VAT export, running in your browser"
            subtitle="A skeletal animation baked to a vertex animation texture and played back in Godot via a shader — no skeleton at runtime. Drag to orbit, scroll to zoom. Switch tabs to compare perf against per-instance skeletal skinning."
          >
            <VATDemo />
          </Section>

          <Section
            id="pipeline"
            eyebrow="Pipeline Overview"
            title="CI/CD pipeline flow for 3D assets"
            subtitle="Use one deterministic flow for local checks and CI runs."
          >
            <div className={styles.pipelineFlow}>
              {pipelineFlow.map((item, index) => (
                <article key={item.title} className={styles.pipelineNode}>
                  <span className={styles.pipelineNodeIndex}>{index + 1}</span>
                  <h3 className={styles.pipelineNodeTitle}>{item.title}</h3>
                  <p className={styles.pipelineNodeBody}>{item.body}</p>
                </article>
              ))}
            </div>

            <article className={styles.pipelinePanel}>
              <h3>Pipeline run in three commands</h3>
              <p>Keep asset handling deterministic from workstation to CI.</p>
              <pre className={styles.pipelineSnippet}>
                <code>{pipelineExamples.scanFixConvert}</code>
              </pre>
            </article>
          </Section>

          <Section
            id="cli"
            eyebrow="CLI + CI"
            title="Use real commands, not toy snippets"
            subtitle="Switch between scan, fix, convert, merge, Docker, and GitHub Actions examples."
          >
            <div className={styles.cliTabs} role="tablist" aria-label="CLI and CI examples">
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
              <CodePanel title={activeCliExample.title} code={activeCliExample.code} label={activeCliExample.language} />
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
            id="features"
            eyebrow="Advanced Features"
            title="Deeper capabilities for specialized workflows"
            subtitle="Keep advanced tooling available without hiding the core CI/CD path."
          >
            <div className={styles.highlightGrid}>
              {highlightFeatures.map((feature) => (
                <FeatureCard
                  key={feature.title}
                  title={feature.title}
                  body={feature.body}
                  tag={feature.title.includes('AI') || feature.title.includes('MCP') ? 'Advanced' : 'Feature'}
                />
              ))}
            </div>

            <div className={styles.previewStrip}>
              <figure className={styles.previewCard}>
                <img src={media.aiMaterials.src} alt={media.aiMaterials.alt} loading="lazy" />
                <figcaption>Material editing workflows for look-dev and technical review.</figcaption>
              </figure>
              <figure className={styles.previewCard}>
                <img src={media.mcpPreview.src} alt={media.mcpPreview.alt} loading="lazy" />
                <figcaption>MCP integration for agent-driven pipeline automation.</figcaption>
              </figure>
            </div>
          </Section>

          <Section
            id="install"
            eyebrow="Install"
            title="Install QtMeshEditor and start scanning"
            subtitle="Choose your platform path: winget, Homebrew, snap, Docker, or release binaries."
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
                <article key={option.platform} className={styles.installMethodCard}>
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
            eyebrow="Open Source Trust"
            title="Credible, technical, and built in public"
            subtitle="Community-driven since 2012 with practical tooling for production asset pipelines."
          >
            <div className={styles.trustMetrics}>
              <a href="https://github.com/fernandotonon/QtMeshEditor/stargazers" target="_blank" rel="noreferrer">
                <img
                  src="https://img.shields.io/github/stars/fernandotonon/QtMeshEditor?style=for-the-badge&label=GitHub%20stars"
                  alt="QtMeshEditor GitHub stars"
                  loading="lazy"
                />
              </a>
              <a href={links.releases} target="_blank" rel="noreferrer">
                <img
                  src="https://img.shields.io/github/v/release/fernandotonon/QtMeshEditor?style=for-the-badge&label=Latest%20release"
                  alt="QtMeshEditor latest release"
                  loading="lazy"
                />
              </a>
              <a href={links.license} target="_blank" rel="noreferrer">
                <img
                  src="https://img.shields.io/github/license/fernandotonon/QtMeshEditor?style=for-the-badge&label=License"
                  alt="QtMeshEditor license badge"
                  loading="lazy"
                />
              </a>
            </div>

            <div className={styles.trustLead}>
              <p className={styles.trustCopy}>Active development since 2012 • MIT license • Community-driven roadmap</p>
            </div>

            <div className={styles.trustGrid}>
              {trustItems.map((item) => (
                <FeatureCard key={item.title} title={item.title} body={item.body} />
              ))}
            </div>
          </Section>
        </main>

        <footer className={styles.footer}>
          <div className={styles.footerInner}>
            <p>QtMeshEditor</p>
            <div className={styles.footerLinks}>
              {footerLinks.map((item) => (
                <a key={item.label} href={item.href} target="_blank" rel="noreferrer">
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
                  <h2 id="install-portal-title" className={styles.installPortalTitle}>
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
                {recommendedInstall ? (
                  <article className={`${styles.installPortalCard} ${styles.installPortalCardRecommended}`}>
                    <div className={styles.installPortalCardTop}>
                      <h3>{recommendedInstall.platform}</h3>
                      <div className={styles.installPortalCardActions}>
                        <span>{recommendedInstall.method}</span>
                        <button
                          type="button"
                          className={styles.installPortalCopyButton}
                          onClick={handleCopyInstallCommand}
                        >
                          {copyState === 'copied' ? 'Copied' : copyState === 'error' ? 'Retry' : 'Copy'}
                        </button>
                      </div>
                    </div>
                    <pre>
                      <code>{recommendedInstall.command}</code>
                    </pre>
                  </article>
                ) : (
                  <p className={styles.installPortalFallback}>
                    OS detection is unavailable in this browser. Use the latest release download links below.
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
          document.body
        )}
    </>
  );
}

export default App;
