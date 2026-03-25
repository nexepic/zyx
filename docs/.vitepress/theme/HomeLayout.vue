<script setup lang="ts">
import { computed, onMounted, onUnmounted, ref } from 'vue'
import { useData } from 'vitepress'

const { lang } = useData()
const isEn = computed(() => lang.value === 'en-US' || lang.value === 'en')

const quickStartLink = computed(() =>
    isEn.value ? '/zyx/en/user-guide/quick-start' : '/zyx/zh/user-guide/quick-start'
)

const canvasRef = ref<HTMLCanvasElement | null>(null)
const isMobile = ref(false)

// === Carousel State ===
const activeFeatureIndex = ref(0)
let cycleTimer: ReturnType<typeof setInterval> | null = null

type AxisDirection = 'x+' | 'x-' | 'y+' | 'y-' | 'z+' | 'z-'

interface AmbientParticle {
  x: number; y: number; z: number;
  vx: number; vy: number; vz: number;
  baseRadius: number; baseBrightness: number;
  axisIndex: 0 | 1 | 2; axisPos: number;
  jitterA: number; jitterB: number;
  color: string; // Precomputed color string
}

interface GlyphParticle {
  lx: number; ly: number; lz: number;
  ax: number; ay: number; az: number;
  x: number; y: number; z: number;
  baseRadius: number; baseBrightness: number;
  color: string; // Precomputed color string
}

interface FeatureData {
  icon: string; titleEn: string; titleZh: string; descEn: string; descZh: string; axis: AxisDirection;
}

const features: FeatureData[] = [
  { icon: 'IO', titleEn: 'Throughput Engine', titleZh: '吞吐引擎', descEn: 'Segmented storage and parallel IO keep heavy workloads stable.', descZh: '分段存储与并行 IO 让高负载查询保持稳定吞吐。', axis: 'x+' },
  { icon: 'TX', titleEn: 'Transactional Core', titleZh: '事务核心', descEn: 'WAL + optimistic control + rollback keep state transitions safe.', descZh: 'WAL、乐观并发与回滚机制共同保障状态转换安全。', axis: 'x-' },
  { icon: 'CY', titleEn: 'Cypher Native', titleZh: 'Cypher 原生', descEn: 'Parser, planner, and optimizer tuned for graph-first semantics.', descZh: '解析、规划、优化链路面向图语义原生设计。', axis: 'y+' },
  { icon: 'VS', titleEn: 'Vector Path', titleZh: '向量路径', descEn: 'DiskANN and quantization integrate ANN into transactional graphs.', descZh: 'DiskANN 与量化流程将 ANN 能力融入事务系统。', axis: 'y-' },
  { icon: 'EM', titleEn: 'Embeddable API', titleZh: '可嵌入 API', descEn: 'Lean C++/C interfaces with no external daemon requirement.', descZh: '轻量 C++/C 接口，无需外部守护进程即可集成。', axis: 'z+' },
  { icon: 'OS', titleEn: 'Open Source', titleZh: '开源协作', descEn: 'MIT-licensed codebase designed for transparent evolution.', descZh: 'MIT 许可，演进路径透明，便于长期协作与维护。', axis: 'z-' }
]

// === Carousel Logic ===
function startAutoCycle() {
  if (isMobile.value) return
  cycleTimer = setInterval(() => {
    activeFeatureIndex.value = (activeFeatureIndex.value + 1) % features.length
  }, 4500) // 4.5s is ideal for reading
}

function stopAutoCycle() {
  if (cycleTimer) clearInterval(cycleTimer)
}

// === Canvas Globals ===
let animationId: number | null = null
let ctx: CanvasRenderingContext2D | null = null
let width = 0; let height = 0; let dpr = 1; let frameCount = 0

let currentMouseYaw = 0; let currentMousePitch = 0
let targetMouseYaw = 0; let targetMousePitch = 0

const ambientParticles: AmbientParticle[] = []
const glyphParticles: GlyphParticle[] = []

const BG = '#0b0f14'
const AXIS_A = '#3a4a5e'
const AXIS_B = '#6b829c'
const FAR_RGB = [30, 40, 52]
const NEAR_RGB = [148, 168, 190]
const AXIS_LENGTH = 550
const MORPH_CYCLE = 2400
const BASE_YAW = -0.2

const clamp = (v: number, min: number, max: number) => Math.max(min, Math.min(max, v))
const smoothstep = (e0: number, e1: number, x: number) => {
  const t = clamp((x - e0) / (e1 - e0), 0, 1)
  return t * t * (3 - 2 * t)
}

function updateMobileFlag() {
  isMobile.value = window.innerWidth <= 768
}

function shuffle<T>(array: T[]) {
  for (let i = array.length - 1; i > 0; i--) {
    const j = Math.floor(Math.random() * (i + 1));
    [array[i], array[j]] = [array[j], array[i]];
  }
}

function createGlyphTargets(count: number) {
  const canvas = document.createElement('canvas')
  const cw = 2000, ch = 1000
  canvas.width = cw; canvas.height = ch
  const textCtx = canvas.getContext('2d', { willReadFrequently: true })
  if (!textCtx) return []

  textCtx.fillStyle = '#fff'
  textCtx.textAlign = 'center'
  textCtx.textBaseline = 'middle'
  textCtx.font = `bold 700px "Space Mono", "Ubuntu Mono", monospace`
  textCtx.fillText('ZYX', cw * 0.5, ch * 0.5)

  const data = textCtx.getImageData(0, 0, cw, ch).data
  const points: Array<{ x: number; y: number }> = []

  let minX = cw, maxX = 0, minY = ch, maxY = 0;
  for (let y = 0; y < ch; y += 7) {
    for (let x = 0; x < cw; x += 7) {
      if (data[(y * cw + x) * 4 + 3] > 120) {
        points.push({ x, y })
        if (x < minX) minX = x; if (x > maxX) maxX = x;
        if (y < minY) minY = y; if (y > maxY) maxY = y;
      }
    }
  }

  if (!points.length) return []
  shuffle(points)

  const exactCenterX = (minX + maxX) / 2;
  const exactCenterY = (minY + maxY) / 2;
  const scale = isMobile.value ? 0.6 : 1.15;
  const targets: Array<{ x: number; y: number; z: number }> = []

  for (let i = 0; i < count; i++) {
    const p = points[i % points.length]
    targets.push({
      x: (p.x - exactCenterX) * scale,
      y: (p.y - exactCenterY) * scale,
      z: (Math.random() - 0.5) * 120
    })
  }
  return targets
}

function resetParticles() {
  ambientParticles.length = 0
  glyphParticles.length = 0

  const ambientCount = isMobile.value ? 500 : 1500
  const glyphCount = isMobile.value ? 400 : 1000

  for (let i = 0; i < ambientCount; i++) {
    const axisIndex = Math.floor(Math.random() * 3) as 0|1|2
    const axisPos = (Math.random() * 2 - 1) * AXIS_LENGTH * 1.5
    const spread = Math.pow(Math.random(), 0.8) * 200 + 10
    const angle = Math.random() * Math.PI * 2
    const brightness = clamp(1 - spread / 250, 0.1, 1)

    // Precompute color
    const r = ~~(FAR_RGB[0] + (NEAR_RGB[0] - FAR_RGB[0]) * brightness)
    const g = ~~(FAR_RGB[1] + (NEAR_RGB[1] - FAR_RGB[1]) * brightness)
    const b = ~~(FAR_RGB[2] + (NEAR_RGB[2] - FAR_RGB[2]) * brightness)

    ambientParticles.push({
      x: 0, y: 0, z: 0, vx: 0, vy: 0, vz: 0,
      baseRadius: Math.random() * 1.0 + 0.5,
      baseBrightness: brightness,
      axisIndex, axisPos,
      jitterA: Math.cos(angle) * spread,
      jitterB: Math.sin(angle) * spread,
      color: `rgb(${r},${g},${b})`
    })
  }

  const targets = createGlyphTargets(glyphCount)
  for (let i = 0; i < glyphCount; i++) {
    const glyph = targets[i] || { x: 0, y: 0, z: 0 }
    const br = 0.7 + Math.random() * 0.3
    const r = ~~(FAR_RGB[0] + (NEAR_RGB[0] - FAR_RGB[0]) * br)
    const g = ~~(FAR_RGB[1] + (NEAR_RGB[1] - FAR_RGB[1]) * br)
    const b = ~~(FAR_RGB[2] + (NEAR_RGB[2] - FAR_RGB[2]) * br)

    glyphParticles.push({
      lx: glyph.x, ly: glyph.y, lz: glyph.z,
      ax: (Math.random() - 0.5) * AXIS_LENGTH * 2,
      ay: (Math.random() - 0.5) * AXIS_LENGTH * 1.2,
      az: (Math.random() - 0.5) * AXIS_LENGTH * 2,
      x: 0, y: 0, z: 0,
      baseRadius: isMobile.value ? 1.0 : 1.2,
      baseBrightness: br,
      color: `rgb(${r},${g},${b})`
    })
  }
}

function resizeCanvas() {
  const canvas = canvasRef.value
  if (!canvas || !ctx) return
  width = window.innerWidth; height = window.innerHeight
  dpr = Math.min(window.devicePixelRatio || 1, 2)
  canvas.width = Math.floor(width * dpr); canvas.height = Math.floor(height * dpr)
  canvas.style.width = `${width}px`; canvas.style.height = `${height}px`
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0)
  updateMobileFlag()
  resetParticles()
}

function projectAxis(x: number, y: number, z: number, cosX: number, sinX: number, cosY: number, sinY: number, cx: number, cy: number, fov: number) {
  const x1 = x * cosY - z * sinY
  const z1 = x * sinY + z * cosY
  const y2 = y * cosX - z1 * sinX
  const z2 = y * sinX + z1 * cosX
  const scale = fov / (fov + z2)
  return { sx: cx + x1 * scale, sy: cy + y2 * scale }
}

function drawAxes(cosX: number, sinX: number, cosY: number, sinY: number, cx: number, cy: number, fov: number, alpha: number) {
  if (!ctx || alpha <= 0.01) return
  ctx.globalAlpha = alpha * 0.4
  const axes = [[1,0,0], [0,1,0], [0,0,1]]
  axes.forEach((axis) => {
    const s = projectAxis(-axis[0]*AXIS_LENGTH, -axis[1]*AXIS_LENGTH, -axis[2]*AXIS_LENGTH, cosX, sinX, cosY, sinY, cx, cy, fov)
    const e = projectAxis(axis[0]*AXIS_LENGTH, axis[1]*AXIS_LENGTH, axis[2]*AXIS_LENGTH, cosX, sinX, cosY, sinY, cx, cy, fov)
    const grad = ctx!.createLinearGradient(s.sx, s.sy, e.sx, e.sy)
    grad.addColorStop(0, AXIS_A); grad.addColorStop(0.5, AXIS_B); grad.addColorStop(1, AXIS_A)
    ctx!.strokeStyle = grad
    ctx!.lineWidth = 1
    ctx!.beginPath(); ctx!.moveTo(s.sx, s.sy); ctx!.lineTo(e.sx, e.sy); ctx!.stroke()
  })
}

function animate() {
  if (!ctx) return
  frameCount += 1

  // Clear frame
  ctx.globalAlpha = 1
  ctx.fillStyle = BG
  ctx.fillRect(0, 0, width, height)

  currentMouseYaw += (targetMouseYaw - currentMouseYaw) * 0.05
  currentMousePitch += (targetMousePitch - currentMousePitch) * 0.05
  const rx = currentMousePitch, ry = BASE_YAW + currentMouseYaw
  const cosX = Math.cos(rx), sinX = Math.sin(rx)
  const cosY = Math.cos(ry), sinY = Math.sin(ry)
  const cx = width * 0.5, cy = height * 0.5, fov = 1000

  const t = (frameCount % MORPH_CYCLE) / MORPH_CYCLE
  const gather = smoothstep(0.40, 0.55, t)
  const release = smoothstep(0.75, 0.90, t)
  const glyphMorph = clamp(gather - release, 0, 1)
  const axisAlpha = smoothstep(0.1, 0.5, 1 - glyphMorph)

  // Ambient particles
  for (let i = 0; i < ambientParticles.length; i++) {
    const p = ambientParticles[i]
    const wobble = 1 + Math.sin(frameCount * 0.005 + p.axisPos * 0.01) * 0.05
    let tx = 0, ty = 0, tz = 0
    if (p.axisIndex === 0) { tx = p.axisPos; ty = p.jitterA * wobble; tz = p.jitterB * wobble }
    else if (p.axisIndex === 1) { tx = p.jitterA * wobble; ty = p.axisPos; tz = p.jitterB * wobble }
    else { tx = p.jitterA * wobble; ty = p.jitterB * wobble; tz = p.axisPos }

    p.x += (tx - p.x) * 0.06; p.y += (ty - p.y) * 0.06; p.z += (tz - p.z) * 0.06

    const x1 = p.x * cosY - p.z * sinY
    const z1 = p.x * sinY + p.z * cosY
    const y2 = p.y * cosX - z1 * sinX
    const z2 = p.y * sinX + z1 * cosX
    if (z2 <= -fov + 20) continue
    const scale = fov / (fov + z2)

    ctx.globalAlpha = (0.2 + p.baseBrightness * 0.4) * axisAlpha
    ctx.fillStyle = p.color
    const size = p.baseRadius * scale * 2
    ctx.fillRect(cx + x1 * scale - size*0.5, cy + y2 * scale - size*0.5, size, size)
  }

  // ZYX glyph particles
  for (let i = 0; i < glyphParticles.length; i++) {
    const p = glyphParticles[i]
    const tx = p.ax + (p.lx - p.ax) * glyphMorph
    const ty = p.ay + (p.ly - p.ay) * glyphMorph
    const tz = p.az + (p.lz - p.az) * glyphMorph

    p.x += (tx - p.x) * 0.08; p.y += (ty - p.y) * 0.08; p.z += (tz - p.z) * 0.08

    const x1 = p.x * cosY - p.z * sinY
    const z1 = p.x * sinY + p.z * cosY
    const y2 = p.y * cosX - z1 * sinX
    const z2 = p.y * sinX + z1 * cosX
    if (z2 <= -fov + 20) continue
    const scale = fov / (fov + z2)

    ctx.globalAlpha = 0.3 + glyphMorph * 0.6
    ctx.fillStyle = p.color
    const size = p.baseRadius * scale * (1 + glyphMorph * 0.3) * 2
    ctx.fillRect(cx + x1 * scale - size*0.5, cy + y2 * scale - size*0.5, size, size)
  }

  drawAxes(cosX, sinX, cosY, sinY, cx, cy, fov, axisAlpha)
  animationId = window.requestAnimationFrame(animate)
}

function handleMouseMove(e: MouseEvent) {
  targetMouseYaw = (e.clientX / width - 0.5) * 0.3
  targetMousePitch = -(e.clientY / height - 0.5) * 0.2
}

onMounted(() => {
  ctx = canvasRef.value?.getContext('2d', { alpha: false })
  if (!ctx) return
  resizeCanvas()
  startAutoCycle() // Start automated billboard
  window.addEventListener('resize', resizeCanvas)
  window.addEventListener('mousemove', handleMouseMove)
  window.addEventListener('mouseleave', () => { targetMouseYaw = 0; targetMousePitch = 0 })
  animationId = window.requestAnimationFrame(animate)
})

onUnmounted(() => {
  if (animationId) window.cancelAnimationFrame(animationId)
  stopAutoCycle()
  window.removeEventListener('resize', resizeCanvas)
  window.removeEventListener('mousemove', handleMouseMove)
})
</script>

<template>
  <div class="home-root">
    <canvas ref="canvasRef" class="bg-canvas"></canvas>

    <div class="overlay-ui">

      <!-- Unified Central Flex Container -->
      <main class="hero-center">
        <!-- Hero Content -->
        <div class="hero-badge">{{ isEn ? 'GRAPH DATABASE ENGINE' : '图数据库引擎' }}</div>
        <h1 class="hero-title" v-if="isEn">Coordinate-native graph runtime</h1>
        <h1 class="hero-title" v-else>坐标原生的图数据库运行时</h1>
        <p class="hero-subtitle" v-if="isEn">
          A unified dimension of transactional consistency and high throughput.<br>
          Designed for uncompromising scale and performance.
        </p>
        <p class="hero-subtitle" v-else>
          构建事务一致性与高吞吐的统一数据维度。<br>
          为无妥协的扩展性与极致性能而原生设计。
        </p>
        <div class="hero-actions">
          <a :href="quickStartLink" class="btn-primary">
            {{ isEn ? 'Get Started' : '快速开始' }}
            <span aria-hidden="true">&rarr;</span>
          </a>
          <a href="https://github.com/nexepic/zyx" target="_blank" rel="noreferrer" class="btn-secondary">
            GitHub ↗
          </a>
        </div>

        <!-- Cyber HUD Carousel (Desktop Only, sits exactly below buttons) -->
        <div v-if="!isMobile" class="hud-carousel" @mouseenter="stopAutoCycle" @mouseleave="startAutoCycle">

          <!-- Sci-Fi Corner Brackets -->
          <span class="hud-bracket bracket-tl"></span>
          <span class="hud-bracket bracket-tr"></span>
          <span class="hud-bracket bracket-bl"></span>
          <span class="hud-bracket bracket-br"></span>

          <!-- Fixed-height viewport locked to prevent jitter -->
          <div class="hud-viewport">
            <transition-group name="data-feed" tag="div">
              <div
                  v-for="(feature, index) in features"
                  :key="feature.titleEn"
                  v-show="index === activeFeatureIndex"
                  class="hud-content"
              >
                <!-- Terminal style icon -->
                <div class="hud-icon">[ {{ feature.icon }} ]</div>

                <!-- Perfectly centered typography -->
                <h3 class="hud-title">{{ isEn ? feature.titleEn : feature.titleZh }}</h3>
                <p class="hud-desc">{{ isEn ? feature.descEn : feature.descZh }}</p>
              </div>
            </transition-group>
          </div>
        </div>
      </main>

      <!-- Original Mobile Section -->
      <div v-if="isMobile" class="mobile-features-row">
        <div v-for="feature in features" :key="feature.titleEn" class="mobile-card">
          <div class="mobile-icon">{{ feature.icon }}</div>
          <h3>{{ isEn ? feature.titleEn : feature.titleZh }}</h3>
          <p>{{ isEn ? feature.descEn : feature.descZh }}</p>
        </div>
      </div>

      <!-- Original Footer -->
      <footer class="home-footer">
        MIT License &copy; 2025 ZYX Contributors
      </footer>
    </div>
  </div>
</template>

<style scoped>
/* Unchanged Original Base Styles */
.home-root {
  position: relative;
  width: 100vw;
  height: 100vh;
  overflow: hidden;
  background: #0b0f14;
  font-family: 'Avenir Next', 'Segoe UI', system-ui, sans-serif;
  color: #e7edf5;
}

.home-root::before {
  content: '';
  position: absolute;
  inset: 0;
  pointer-events: none;
  background: radial-gradient(circle at 50% 50%, rgba(11, 15, 20, 0.6) 0%, transparent 60%);
  z-index: 1;
}

.bg-canvas {
  position: absolute;
  inset: 0;
  z-index: 0;
  pointer-events: auto;
}

.overlay-ui {
  position: absolute;
  inset: 0;
  z-index: 2;
  display: flex;
  flex-direction: column;
  pointer-events: none;
}

/*
  === OVERALL POSITION CONTROL ===
  Modify `top: 52%;` below to move the entire content block (Text + Buttons + Carousel) up or down.
*/
.hero-center {
  position: absolute;
  top: 52%;
  left: 50%;
  transform: translate(-50%, -50%);
  width: min(860px, 92vw);
  display: flex;
  flex-direction: column;
  align-items: center;
  text-align: center;
  pointer-events: auto;
}

.hero-badge {
  display: inline-flex;
  padding: 0.35rem 1rem;
  margin-bottom: 1.2rem;
  border-radius: 50px;
  border: 1px solid rgba(122, 144, 170, 0.3);
  background: rgba(22, 30, 39, 0.4);
  color: #aec0d2;
  font-size: 0.72rem;
  font-weight: 500;
  letter-spacing: 0.16em;
  text-transform: uppercase;
  backdrop-filter: blur(4px);
}

.hero-title {
  margin: 0;
  font-size: clamp(2rem, 4vw, 3.8rem);
  font-weight: 600;
  letter-spacing: 0.02em;
  color: #e7edf5;
  line-height: 1.15;
}

.hero-subtitle {
  margin: 1.2rem 0 2rem;
  color: #9db0c4;
  font-size: clamp(0.95rem, 1.4vw, 1.05rem);
  line-height: 1.6;
}

.hero-actions {
  display: flex;
  gap: 1rem;
  margin-bottom: 3.5rem;
}

/*
  === BUTTON STYLES ===
  Removed the upward hover movement (translateY) to fix
  the browser rendering/compositing flicker bug.
*/
.btn-primary, .btn-secondary {
  display: inline-flex;
  align-items: center;
  justify-content: center;
  gap: 0.5rem;
  min-width: 140px;
  padding: 0.75rem 1.4rem;
  border-radius: 8px;
  font-size: 0.88rem;
  text-decoration: none;
  transition: all 0.25s ease;
  backdrop-filter: blur(4px);
}

.btn-primary {
  color: #e7edf5;
  border: 1px solid rgba(122, 144, 170, 0.7);
  background: rgba(35, 48, 63, 0.6);
}

.btn-primary:hover {
  background: #778ea7;
  color: #0b0f14;
}

.btn-secondary {
  color: #b8c7d6;
  border: 1px solid rgba(122, 144, 170, 0.3);
  background: rgba(24, 34, 44, 0.4);
}

.btn-secondary:hover {
  background: rgba(118, 142, 167, 0.2);
  color: #fff;
}

/* === Cyber HUD Carousel (Flows naturally in Flexbox) === */
.hud-carousel {
  position: relative;
  width: 100%;
  max-width: 580px;
  padding: 1.5rem 0;
  background: radial-gradient(ellipse at center, rgba(148, 168, 190, 0.06) 0%, transparent 70%);
}

/* Sci-Fi Corner Brackets */
.hud-bracket {
  position: absolute;
  width: 12px;
  height: 12px;
  border-color: rgba(122, 144, 170, 0.4);
  border-style: solid;
  transition: border-color 0.3s ease;
}

.hud-carousel:hover .hud-bracket {
  border-color: rgba(148, 168, 190, 0.8);
  box-shadow: 0 0 8px rgba(148, 168, 190, 0.2);
}

.bracket-tl { top: 0; left: 0; border-width: 1px 0 0 1px; }
.bracket-tr { top: 0; right: 0; border-width: 1px 1px 0 0; }
.bracket-bl { bottom: 0; left: 0; border-width: 0 0 1px 1px; }
.bracket-br { bottom: 0; right: 0; border-width: 0 1px 1px 0; }

/* Fixed viewport guarantees zero vertical jitter */
.hud-viewport {
  position: relative;
  width: 100%;
  height: 95px; /* Locked height */
  overflow: hidden;
}

/* Absolute positioning to stack animations precisely */
.hud-content {
  position: absolute;
  top: 0; left: 0; right: 0;
  width: 100%;
  height: 100%;
  display: flex;
  flex-direction: column;
  align-items: center;
  text-align: center;
}

/* Command-line style icon */
.hud-icon {
  margin-bottom: 0.6rem;
  color: #94a8be;
  font-size: 0.75rem;
  font-weight: 600;
  font-family: 'Space Mono', 'Ubuntu Mono', monospace;
  letter-spacing: 0.15em;
  text-shadow: 0 0 10px rgba(148, 168, 190, 0.4);
}

/* Tech Typography */
.hud-title {
  margin: 0 0 0.4rem;
  font-size: 1.1rem;
  font-weight: 600;
  color: #e7edf5;
  letter-spacing: 0.03em;
}

.hud-desc {
  margin: 0;
  font-size: 0.85rem;
  line-height: 1.5;
  color: #8a9bb0;
  max-width: 480px; /* Constrains line length for elegant reading */
}

/* Data-feed vertical blur slide (Sci-fi feel) */
.data-feed-enter-active,
.data-feed-leave-active {
  transition: all 0.6s cubic-bezier(0.16, 1, 0.3, 1);
}
.data-feed-enter-from {
  opacity: 0;
  transform: translateY(15px) scale(0.98);
  filter: blur(8px);
}
.data-feed-leave-to {
  opacity: 0;
  transform: translateY(-15px) scale(1.02);
  filter: blur(8px);
}

/* Original Mobile Bottom Row */
.mobile-features-row {
  position: absolute;
  bottom: 2rem;
  left: 0;
  right: 0;
  display: flex;
  gap: 0.8rem;
  padding: 0 1.2rem;
  overflow-x: auto;
  pointer-events: auto;
  scrollbar-width: none;
}

.mobile-features-row::-webkit-scrollbar {
  display: none;
}

.mobile-card {
  flex: 0 0 80%;
  max-width: 280px;
  background: rgba(16, 22, 30, 0.75);
  backdrop-filter: blur(10px);
  border: 1px solid rgba(122, 144, 170, 0.2);
  border-radius: 12px;
  padding: 1.1rem;
}

.mobile-icon {
  display: inline-block;
  padding: 0.2rem 0.6rem;
  border-radius: 50px;
  background: rgba(122, 144, 170, 0.15);
  color: #c5d2de;
  font-size: 0.65rem;
  margin-bottom: 0.6rem;
}

.mobile-card h3 { margin: 0 0 0.3rem; font-size: 0.9rem; color: #e7edf5; }
.mobile-card p { margin: 0; font-size: 0.8rem; color: #8a9bb0; line-height: 1.45; }

/* Original Footer */
.home-footer {
  position: absolute;
  bottom: 0.8rem;
  left: 50%;
  transform: translateX(-50%);
  font-size: 0.7rem;
  color: #566b82;
}

/* Responsive constraints */
@media (max-width: 768px) {
  .hero-center { top: 40%; width: calc(100vw - 2.4rem); }
  .hero-title { font-size: 1.6rem; }
  .hero-actions { flex-direction: column; width: 100%; margin-bottom: 0; }
  .btn-primary, .btn-secondary { width: 100%; }
}
</style>