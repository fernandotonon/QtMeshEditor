import styles from './FeatureCard.module.css';

export default function FeatureCard({ title, body, tag }) {
  return (
    <article className={styles.card}>
      {tag ? <p className={styles.tag}>{tag}</p> : null}
      <h3 className={styles.title}>{title}</h3>
      <p className={styles.body}>{body}</p>
    </article>
  );
}
