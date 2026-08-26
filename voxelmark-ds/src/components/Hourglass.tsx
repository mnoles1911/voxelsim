import type { CSSProperties } from 'react';

// --- the mock's SVG space, verbatim -----------------------------------------
// These are the numbers LoadingHourglass.gd draws in, and the reason this
// component is an SVG rather than a picture: the shape was authored as vector
// geometry in a 40x60 box and has stayed that way through two ports.
const W = 40;
const H = 60;
const WAIST_X = 20;
const WAIST_Y = 30;

// The mock uses 38 grains; the shipped build cut it to 24 to reduce per-frame
// work during chunk streaming -- the loading screen is on screen precisely when
// the frame budget is worst. Kept at 24.
const MAX_GRAINS = 24;

const GRAIN_BRIGHT = 'var(--vm-sand-bright)';
const GRAIN_ALT = '#e8b850';

/** Deterministic PRNG, so the same progress always draws the same grains. */
function mulberry32(seed: number) {
  let a = seed >>> 0;
  return () => {
    a = (a + 0x6d2b79f5) >>> 0;
    let t = Math.imul(a ^ (a >>> 15), 1 | a);
    t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t;
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
}

export interface HourglassProps {
  /** How far through the load, 0 to 1. Drives the sand in both bulbs. */
  progress?: number;
  /**
   * Animate the falling grains and the +/-2px vertical bob. On by default;
   * turn it off for a still frame.
   */
  animated?: boolean;
  /** Seed for the grain scatter. Same seed and progress draw identical grains. */
  seed?: number;
  className?: string;
  style?: CSSProperties;
}

/**
 * The loading screen's hourglass: brass caps and pillars, an asymmetric glass
 * diamond, sand draining from the top bulb onto a growing mound below, and
 * grains falling through the waist.
 *
 * Drawn as vector geometry in the original 40x60 space, so it stays crisp at
 * any size. **The caps and pillars overhang the glass on every side and must
 * not be clipped** -- the overhang is the shape.
 *
 * The glass diamond is deliberately asymmetric (the right waist sits one unit
 * further out than the left). That is character, not a bug, and it has
 * survived every port on purpose.
 *
 * @example
 * <Hourglass progress={0.42} />
 * <Hourglass progress={0.9} animated={false} />
 */
export function Hourglass({
  progress = 0.5,
  animated = true,
  seed = 1,
  className,
  style,
}: HourglassProps) {
  const p = Math.min(Math.max(progress, 0), 1);

  // --- top sand: a triangle collapsing onto the waist as it drains ---------
  const surfaceY = 0.5 + p * 28.5;
  const topHalfWidth = (1 - p) * 19.5;

  // --- bottom mound: a triangle growing from the floor up toward the waist --
  const apexY = H - p * (H - WAIST_Y);
  const moundHalfWidth = p * (W * 0.5);
  // A tiny mound gets a clean triangle: the apex bevel below overshoots the
  // floor at that size and self-intersects.
  const bevel = moundHalfWidth * 0.06;
  const bevelY = Math.min(apexY + 0.5, H - 0.1);
  const moundPoints =
    moundHalfWidth < 2
      ? `${WAIST_X - moundHalfWidth},${H} ${WAIST_X + moundHalfWidth},${H} ${WAIST_X},${apexY}`
      : `${WAIST_X - moundHalfWidth},${H} ${WAIST_X + moundHalfWidth},${H} ` +
        `${WAIST_X + bevel},${bevelY} ${WAIST_X},${apexY} ${WAIST_X - bevel},${bevelY}`;

  // --- falling grains -----------------------------------------------------
  // Spawned at the waist and accelerating downward, as the physics tick does.
  // Positions are laid out along that path rather than simulated per frame:
  // the shape at any instant is the same, and a design system should not carry
  // a physics loop into every design built with it.
  const grains: Array<{ x: number; y: number; size: number; fill: string; drop: number; delay: number }> = [];
  if (p > 0.005 && p < 0.995) {
    const rand = mulberry32(seed);
    const fallSpan = Math.max(apexY - WAIST_Y, 0.5);
    for (let i = 0; i < MAX_GRAINS; i += 1) {
      const t = (i + 1) / (MAX_GRAINS + 1);
      grains.push({
        x: WAIST_X + (rand() - 0.5) * 0.6,
        // t^2 rather than t: constant acceleration, so grains bunch near the
        // waist and stretch out as they fall, which is what the eye reads as
        // gravity.
        y: WAIST_Y + fallSpan * t * t,
        size: rand() < 0.4 ? 0.9 : 0.7,
        fill: rand() < 0.5 ? GRAIN_BRIGHT : GRAIN_ALT,
        drop: fallSpan * (1 - t * t),
        delay: -t * 1.1,
      });
    }
  }

  const classes = [
    'vm-hourglass',
    animated ? 'vm-hourglass--bobbing' : null,
    className,
  ]
    .filter(Boolean)
    .join(' ');

  return (
    <svg
      className={classes}
      viewBox={`0 0 ${W} ${H}`}
      style={style}
      shapeRendering="crispEdges"
      role="img"
      aria-label={`Loading, ${Math.floor(p * 100)} percent`}
    >
      {/* --- brass pillars, behind everything ----------------------------- */}
      {[-4, W + 2].map((x) => (
        <g key={x}>
          <rect x={x} y={2} width={2} height={18.667} fill="var(--vm-brass1)" />
          <rect x={x} y={20.667} width={2} height={18.667} fill="var(--vm-brass3)" />
          <rect x={x} y={39.333} width={2} height={18.667} fill="var(--vm-brass4)" />
          <rect x={x} y={2} width={2} height={56} fill="none" stroke="#000" strokeWidth={1} />
        </g>
      ))}

      {/* --- the glass diamond -------------------------------------------
          "0,0 40,0 21,30 40,60 0,60 19,30" -- asymmetric on purpose. */}
      <polygon
        points={`0,0 ${W},0 21,${WAIST_Y} ${W},${H} 0,${H} 19,${WAIST_Y}`}
        fill="rgba(245, 208, 110, 0.05)"
      />

      {/* --- top sand ----------------------------------------------------- */}
      {p < 0.97 ? (
        <polygon
          points={`${WAIST_X - topHalfWidth},${surfaceY} ${WAIST_X + topHalfWidth},${surfaceY} ${WAIST_X},29.5`}
          fill="var(--vm-sand-mid)"
        />
      ) : null}

      {/* --- bottom mound ------------------------------------------------- */}
      {p > 0.005 ? <polygon points={moundPoints} fill="var(--vm-sand-deep)" /> : null}

      {/* --- falling grains ----------------------------------------------- */}
      {grains.map((g, i) => (
        <rect
          key={i}
          className={animated ? 'vm-grain' : undefined}
          x={g.x - g.size / 2}
          y={g.y - g.size / 2}
          width={g.size}
          height={g.size}
          fill={g.fill}
          style={
            animated
              ? ({
                  '--vm-grain-drop': `${g.drop}`,
                  animationDelay: `${g.delay}s`,
                } as CSSProperties)
              : undefined
          }
        />
      ))}

      {/* --- glass outline, over the sand --------------------------------- */}
      <polyline
        points={`0,0 ${W},0 21,${WAIST_Y} ${W},${H} 0,${H} 19,${WAIST_Y} 0,0`}
        fill="none"
        stroke="var(--vm-brass1)"
        strokeWidth={1}
      />

      {/* --- brass caps, drawn last so they sit over the pillars ---------- */}
      {[-3, H - 3].map((y) => (
        <g key={y}>
          <rect x={-6} y={y} width={W + 12} height={3} fill="var(--vm-brass1)" />
          <rect x={-6} y={y + 3} width={W + 12} height={3} fill="var(--vm-brass3)" />
          <rect x={-6} y={y} width={W + 12} height={6} fill="none" stroke="#000" strokeWidth={1} />
        </g>
      ))}
    </svg>
  );
}
