import styles from './App.module.css';
import ButtonLink from './components/ButtonLink';
import CodePanel from './components/CodePanel';
import FeatureCard from './components/FeatureCard';
import InstallCard from './components/InstallCard';
import Section from './components/Section';
import {
  comparisonItems,
  footerLinks,
  hero,
  highlightFeatures,
  installOptions,
  links,
  media,
  mergeSteps,
  pipelineExamples,
  proofPoints,
  trustItems,
  useCases
} from './data/content';

function App() {
  return (
    <div className={styles.page}>
      <div className={styles.backdrop} aria-hidden="true" />

      <main className={styles.main}>
        <header className={`${styles.hero} reveal`}>
          <div className={styles.heroCopy}>
            <p className={styles.kicker}>QtMeshEditor</p>
            <h1 className={styles.heroTitle}>{hero.title}</h1>
            <p className={styles.heroSubtitle}>{hero.subtitle}</p>

            <div className={styles.ctaRow}>
              <ButtonLink href={links.releases}>{hero.ctaPrimary}</ButtonLink>
              <ButtonLink href={links.github} variant="secondary">
                {hero.ctaSecondary}
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
            <img className={styles.heroImage} src={media.mergeDemo.src} alt={media.mergeDemo.alt} />
            <figcaption className={styles.mediaCaption}>Merge workflow in action</figcaption>
          </figure>
        </header>

        <Section
          id="demo"
          eyebrow="Visual Proof"
          title="Merge animation clips without pipeline friction"
          subtitle="The main workflow stays focused: load assets, merge clips, export to the format your engine needs."
        >
          <div className={styles.demoLayout}>
            <figure className={`${styles.demoMain} reveal`}>
              <img className={styles.demoImage} src={media.mergeDemo.src} alt={media.mergeDemo.alt} loading="lazy" />
            </figure>

            <ol className={styles.steps}>
              {mergeSteps.map((step, index) => (
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

          <div className={styles.previewStrip}>
            <figure className={styles.previewCard}>
              <img src={media.skeletonPreview.src} alt={media.skeletonPreview.alt} loading="lazy" />
              <figcaption>Skeleton and weight inspection</figcaption>
            </figure>
            <figure className={styles.previewCard}>
              <img src={media.aiMaterials.src} alt={media.aiMaterials.alt} loading="lazy" />
              <figcaption>AI-assisted materials</figcaption>
            </figure>
            <figure className={styles.previewCard}>
              <img src={media.mcpPreview.src} alt={media.mcpPreview.alt} loading="lazy" />
              <figcaption>MCP tool integration</figcaption>
            </figure>
          </div>
        </Section>

        <Section
          id="use-cases"
          eyebrow="Core Workflows"
          title="Built for daily gamedev asset tasks"
          subtitle="QtMeshEditor prioritizes real production tasks over generic feature sprawl."
        >
          <div className={styles.useCaseGrid}>
            {useCases.map((item) => (
              <FeatureCard key={item.title} title={item.title} body={item.body} tag="Core" />
            ))}
          </div>
        </Section>

        <Section
          id="cli"
          eyebrow="Scriptable Pipeline"
          title="Use qtmesh in local scripts, Docker, and CI"
          subtitle="Run the same commands locally and in build pipelines to keep asset processing repeatable and team-friendly."
        >
          <div className={styles.cliIntro}>
            <p>
              The <code>qtmesh</code> CLI ships with QtMeshEditor and is designed for production automation.
              Use it for format conversion, asset inspection, and animation merge tasks without opening the GUI.
            </p>
            <p>
              Docker image and GitHub Action support are available for containerized workflows and CI/CD pipelines.
            </p>
          </div>

          <div className={styles.codeGrid}>
            <CodePanel title="Inspect meshes" code={pipelineExamples.inspect} />
            <CodePanel title="Convert formats" code={pipelineExamples.convert} />
            <CodePanel title="Merge animations" code={pipelineExamples.merge} />
            <CodePanel title="Docker CLI" code={pipelineExamples.docker} label="docker" />
            <CodePanel title="GitHub Actions" code={pipelineExamples.githubAction} label="ci" />
          </div>

          <div className={styles.cliLinks}>
            <a href={links.actions} target="_blank" rel="noreferrer">
              GitHub Action
            </a>
            <a href={links.releases} target="_blank" rel="noreferrer">
              Latest release binaries
            </a>
            <a href={links.github} target="_blank" rel="noreferrer">
              Full CLI docs on GitHub
            </a>
          </div>
        </Section>

        <Section
          id="comparison"
          eyebrow="Why It Works"
          title="Pick the right mode for each step"
          subtitle="Manual edits, scripted tasks, and CI pipelines can all use the same toolchain."
        >
          <div className={styles.comparisonGrid}>
            {comparisonItems.map((item) => (
              <FeatureCard key={item.title} title={item.title} body={item.body} />
            ))}
          </div>
        </Section>

        <Section
          id="features"
          eyebrow="Extended Features"
          title="Advanced capabilities when you need them"
          subtitle="AI and integration features are available without obscuring the core asset workflow."
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
        </Section>

        <Section
          id="install"
          eyebrow="Install"
          title="Install QtMeshEditor your way"
          subtitle="Desktop packages for creators, plus Docker for automation-first pipelines."
        >
          <div className={styles.installGrid}>
            {installOptions.map((item) => (
              <InstallCard
                key={item.platform}
                platform={item.platform}
                method={item.method}
                command={item.command}
              />
            ))}
          </div>
        </Section>

        <Section
          id="trust"
          eyebrow="Open Source Trust"
          title="Built in public for long-term production use"
          subtitle="Used by developers around the world for practical 3D asset preparation and pipeline automation."
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
  );
}

export default App;
