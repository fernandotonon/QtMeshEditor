import styles from './ButtonLink.module.css';

export default function ButtonLink({ href, children, variant = 'primary' }) {
  const className = `${styles.button} ${styles[variant] || styles.primary}`;
  const externalLink = /^https?:\/\//i.test(href);

  return (
    <a
      className={className}
      href={href}
      target={externalLink ? '_blank' : undefined}
      rel={externalLink ? 'noreferrer' : undefined}
    >
      {children}
    </a>
  );
}
