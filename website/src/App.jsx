import { useEffect, useMemo, useRef, useState } from 'react';
import { createPortal } from 'react-dom';
import styles from './App.module.css';
import ButtonLink from './components/ButtonLink';
import CodePanel from './components/CodePanel';
import FeatureCard from './components/FeatureCard';
import Section from './components/Section';
import {
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
  useCases
} from './data/content';

const PLATFORM_BY_OS = {
  windows: 'Windows',
  macos: 'macOS',
  linux: 'Linux'
};

const FOCUSABLE_SELECTOR =
  'a[href], button:not([disabled]), textarea:not([disabled]), input:not([disabled]), select:not([disabled]), [tabindex]:not([tabindex="-1"])';
const QTMESH_RELEASES_LATEST_API = 'https://api.github.com/repos/fernandotonon/QtMeshEditor/releases/latest';
const QTMESH_ACTION_REF_FALLBACK = 'fernandotonon/QtMeshEditor@v1';

function actionRefFromTag(tagName) {
  const tag = String(tagName || '').trim();
  if (!tag || !/^v?\d/.test(tag)) return QTMESH_ACTION_REF_FALLBACK;
  return `fernandotonon/QtMeshEditor@${tag}`;
}

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
  const [qtmeshActionRef, setQtmeshActionRef] = useState(QTMESH_ACTION_REF_FALLBACK);
  const portalDialogRef = useRef(null);
  const portalTriggerRef = useRef(null);
  const detectedOs = useMemo(detectVisitorOs, []);
  const detectedPlatform = PLATFORM_BY_OS[detectedOs];
  const recommendedInstall = useMemo(
    () => installOptions.find((item) => item.platform === detectedPlatform) || null,
    [detectedPlatform]
  );
  const recommendedStore = recommendedInstall ? getStoreLabel(recommendedInstall.method) : null;
  const primaryCtaLabel = recommendedStore ? `Install via ${recommendedStore}` : 'Open Install Portal';
  const githubActionExample = useMemo(
    () => pipelineExamples.githubAction.replace('__QTMESH_ACTION_REF__', qtmeshActionRef),
    [qtmeshActionRef]
  );

  useEffect(() => {
    let cancelled = false;
    (async () => {
      try {
        const res = await fetch(QTMESH_RELEASES_LATEST_API, {
          headers: { Accept: 'application/vnd.github+json' },
        });
        if (!res.ok) return;
        const data = await res.json();
        if (cancelled) return;
        setQtmeshActionRef(actionRefFromTag(data?.tag_name));
      } catch (_e) {
        // Keep fallback ref when GitHub API is unavailable.
      }
    })();

    return () => {
      cancelled = true;
    };
  }, []);

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

  return (
    <>
      <div className={styles.page}>
        <div className={styles.backdrop} aria-hidden="true" />

        <main className={styles.main}>
          <header className={`${styles.hero} reveal`}>
            <div className={styles.heroCopy}>
              <p className={styles.kicker}>QtMeshEditor</p>
              <h1 className={styles.heroTitle}>{hero.title}</h1>
              <p className={styles.heroSubtitle}>{hero.subtitle}</p>

              <div className={styles.ctaRow}>
                <button
                  type="button"
                  className={`${styles.ctaButton} ${styles.ctaButtonPrimary}`}
                  onClick={handleOpenInstallPortal}
                >
                  {primaryCtaLabel}
                </button>
                <ButtonLink href={links.github} variant="secondary">
                  {hero.ctaSecondary}
                </ButtonLink>
                <ButtonLink href={links.docs} variant="secondary">
                  {hero.ctaDocs}
                </ButtonLink>
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
              <img className={styles.heroImage} src={media.skeletonPreview.src} alt={media.skeletonPreview.alt} />
              <figcaption className={styles.mediaCaption}>Pipeline validation and inspection workflow</figcaption>
            </figure>
          </header>

          <Section
            id="qtmesh-cloud-badges"
            eyebrow="Main Topic"
            title="QtMesh Cloud badges and scan history"
            subtitle="Register your repository in QtMesh Cloud and publish real CI scan results as live SVG badges for status, errors, and warnings."
          >
            <div className={styles.pipelineFlow}>
              {cloudBadgeSteps.map((item, index) => (
                <article key={item.title} className={styles.pipelineNode}>
                  <span className={styles.pipelineNodeIndex}>{index + 1}</span>
                  <h3 className={styles.pipelineNodeTitle}>{item.title}</h3>
                  <p className={styles.pipelineNodeBody}>{item.body}</p>
                </article>
              ))}
            </div>

            <div className={styles.codeGrid}>
              <CodePanel title="GitHub Actions: upload scan report" code={cloudBadgeUploadExample} label="yaml" />
              <CodePanel title="README badge snippet" code={cloudBadgeMarkdown} label="md" />
            </div>

            <div className={styles.cliLinks}>
              <a href={links.qtmeshCloud} target="_blank" rel="noreferrer">
                Register on QtMesh Cloud
              </a>
              <a href={`${links.qtmeshCloudApi}/v1/u/u-28680b9c/p/qtmesheditor/badges/qtmesh-status.svg`} target="_blank" rel="noreferrer">
                View live badge example
              </a>
              <a href={links.docs} target="_blank" rel="noreferrer">
                Read integration docs
              </a>
            </div>
          </Section>

          <Section
            id="pipeline"
            eyebrow="Pipeline Overview"
            title="CI/CD for 3D assets"
            subtitle="One toolchain for scan, validate, fix, convert, and publish across local scripts and automation workflows. Scan and validation are being expanded as first-class pipeline checks."
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

            <div className={styles.demoLayout}>
              <figure className={`${styles.demoMain} reveal`}>
                <img
                  className={styles.demoImage}
                  src={media.pipelineCiCd.src}
                  alt={media.pipelineCiCd.alt}
                  loading="lazy"
                />
              </figure>

              <article className={styles.pipelinePanel}>
                <h3>Pipeline run in three commands</h3>
                <p>
                  Use the same command sequence in local checks and CI jobs to keep asset handling deterministic.
                </p>
                <pre className={styles.pipelineSnippet}>
                  <code>{pipelineExamples.scanFixConvert}</code>
                </pre>
              </article>
            </div>
          </Section>

          <Section
            id="use-cases"
            eyebrow="Core Use Cases"
            title="Built for real asset production tasks"
            subtitle="Pipeline-focused workflows for technical artists, indie teams, and studios shipping content continuously."
          >
            <div className={styles.useCaseGrid}>
              {useCases.map((item) => (
                <FeatureCard key={item.title} title={item.title} body={item.body} tag="Pipeline" />
              ))}
            </div>
          </Section>

          <Section
            id="cli"
            eyebrow="CLI + CI"
            title="Script your 3D asset pipeline"
            subtitle="Run the same `qtmesh` operations locally, in Docker, and inside GitHub Actions."
          >
            <div className={styles.cliIntro}>
              <p>
                Repo-wide scan and validation commands are expanding. The examples below show the intended pipeline
                shape while keeping fix, convert, merge, and automation fully scriptable today.
              </p>
            </div>

            <div className={styles.codeGrid}>
              <CodePanel title="Scan repo assets" code={pipelineExamples.scan} label="scan" />
              <CodePanel title="Fix and optimize" code={pipelineExamples.fix} label="fix" />
              <CodePanel title="Convert formats" code={pipelineExamples.convert} label="convert" />
              <CodePanel title="Merge animations" code={pipelineExamples.merge} label="anim" />
              <CodePanel title="Docker" code={pipelineExamples.docker} label="docker" />
              <CodePanel title="GitHub Actions" code={githubActionExample} label="ci" />
            </div>

            <div className={styles.cliLinks}>
              <a href={links.actions} target="_blank" rel="noreferrer">
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
            id="mixamo"
            eyebrow="Animation Merge Workflow"
            title="Fast Mixamo and Unreal animation merging"
            subtitle="Use QtMeshEditor as a practical entry-point workflow, then plug output directly into the broader pipeline."
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
          </Section>

          <Section
            id="features"
            eyebrow="Advanced Features"
            title="Extended capabilities for specialized workflows"
            subtitle="Advanced tools stay available without overshadowing the core pipeline flow."
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
                <figcaption>MCP integration for advanced agent-driven automation.</figcaption>
              </figure>
            </div>
          </Section>

          <Section
            id="install"
            eyebrow="Install"
            title="Install QtMeshEditor your way"
            subtitle="Store-first installs with winget, Homebrew, and snap, plus Docker and release binaries."
          >
            <div className={styles.installPortalEntry}>
              <p>
                Store options include <code>winget</code>, <code>Homebrew</code>, and <code>snap</code>. You can also
                use Docker or download binaries from the latest release.
              </p>
              <div className={styles.installPortalEntryActions}>
                <button
                  type="button"
                  className={`${styles.ctaButton} ${styles.ctaButtonPrimary}`}
                  onClick={handleOpenInstallPortal}
                >
                  Open Install Portal
                </button>
                <a href={links.releases} target="_blank" rel="noreferrer">
                  Download from latest release
                </a>
              </div>
            </div>
          </Section>

          <Section
            id="trust"
            eyebrow="Open Source Trust"
            title="Built in public for long-term production use"
            subtitle="Open source, multi-platform, and focused on practical pipeline outcomes for small teams and studios."
          >
            <div className={styles.trustLead}>
              <a href="https://github.com/fernandotonon/QtMeshEditor/stargazers" target="_blank" rel="noreferrer">
                <img
                  className={styles.starBadge}
                  src="https://img.shields.io/github/stars/fernandotonon/QtMeshEditor.svg?style=social&label=Star"
                  alt="GitHub stars for QtMeshEditor"
                  loading="lazy"
                />
              </a>
              <p className={styles.trustCopy}>Active development since 2012 • MIT license • Community-driven roadmap</p>
            </div>

            <div className={styles.trustGrid}>
              {trustItems.map((item) => (
                <FeatureCard key={item.title} title={item.title} body={item.body} />
              ))}
            </div>

            <nav className={styles.communityLinks} aria-label="Community links">
              {footerLinks.map((item) => (
                <a key={item.label} href={item.href} target="_blank" rel="noreferrer">
                  {item.label}
                </a>
              ))}
            </nav>
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
