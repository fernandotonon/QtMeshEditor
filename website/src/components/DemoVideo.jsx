/** Native controls keep every demo keyboard accessible and let visitors pause motion.
 * The poster is the only media fetched until playback is requested. */
export default function DemoVideo({ src, poster, label }) {
  return (
    <video
      src={src}
      poster={poster}
      controls
      muted
      loop
      playsInline
      preload="none"
      aria-label={label}
    >
      Your browser does not support this video.{' '}
      <a href={src}>Download the demonstration</a>.
    </video>
  );
}
