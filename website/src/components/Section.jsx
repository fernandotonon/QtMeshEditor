import styles from './Section.module.css';

export default function Section({ id, eyebrow, title, subtitle, children }) {
  return (
    <section id={id} className={styles.section}>
      <div className={styles.header}>
        {eyebrow ? <p className={styles.eyebrow}>{eyebrow}</p> : null}
        {title ? <h2 className={styles.title}>{title}</h2> : null}
        {subtitle ? <p className={styles.subtitle}>{subtitle}</p> : null}
      </div>
      {children}
    </section>
  );
}
