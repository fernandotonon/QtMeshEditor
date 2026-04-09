import styles from './InstallCard.module.css';

export default function InstallCard({ platform, method, command }) {
  return (
    <article className={styles.card}>
      <div className={styles.topRow}>
        <h3 className={styles.platform}>{platform}</h3>
        <span className={styles.method}>{method}</span>
      </div>
      <pre className={styles.command}>
        <code>{command}</code>
      </pre>
    </article>
  );
}
