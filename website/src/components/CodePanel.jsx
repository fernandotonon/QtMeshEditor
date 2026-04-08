import styles from './CodePanel.module.css';

export default function CodePanel({ title, code, label = 'terminal' }) {
  return (
    <article className={styles.panel}>
      <div className={styles.topBar}>
        <span className={styles.dot} />
        <span className={styles.dot} />
        <span className={styles.dot} />
        <span className={styles.label}>{label}</span>
      </div>
      <h3 className={styles.title}>{title}</h3>
      <pre className={styles.code}>
        <code>{code}</code>
      </pre>
    </article>
  );
}
