import useMeasuredHeight from '../hooks/useMeasuredHeight';
import styles from './Section.module.css';

export default function Section({ id, eyebrow, title, subtitle, children }) {
  const [sectionRef, height] = useMeasuredHeight();

  // Each section gets a hover-visible `#`-link next to its title, so a
  // reader can grab a shareable URL (e.g. `/#vat-demo`) without
  // resorting to dev-tools. The deep-link effect in App.jsx handles
  // scrolling to whatever fragment the URL points at — both on initial
  // load and on subsequent `hashchange` events.
  return (
    <section
      id={id}
      ref={sectionRef}
      className={styles.section}
      style={{ '--panel-height': height ? `${height}px` : '100dvh' }}
    >
      <div className={styles.header}>
        {eyebrow ? <p className={styles.eyebrow}>{eyebrow}</p> : null}
        {title ? (
          <h2 className={styles.title}>
            {title}
            {id ? (
              <a
                href={`#${id}`}
                className={styles.anchorLink}
                aria-label={`Permalink to ${typeof title === 'string' ? title : 'this section'}`}
                title="Copy link to this section"
              >
                #
              </a>
            ) : null}
          </h2>
        ) : null}
        {subtitle ? <p className={styles.subtitle}>{subtitle}</p> : null}
      </div>
      {children}
    </section>
  );
}
