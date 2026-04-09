import styles from './ButtonLink.module.css';

export default function ButtonLink({ href, children, variant = 'primary' }) {
  const className = `${styles.button} ${styles[variant] || styles.primary}`;

  return (
    <a className={className} href={href} target="_blank" rel="noreferrer">
      {children}
    </a>
  );
}
