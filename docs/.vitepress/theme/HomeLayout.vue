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
const hoveredNodeIndex = ref<number | null>(null)

type AxisDirection = 'x+' | 'x-' | 'y+' | 'y-' | 'z+' | 'z-'
type AxisLetter = 'X' | 'Y' | 'Z'

// Particle structures for high performance
interface AmbientParticle {
  x: number; y: number; z: number;
  vx: number; vy: number; vz: number;
  baseRadius: number; baseBrightness: number;
  axisIndex: 0 | 1 | 2; axisPos: number;
  jitterA: number; jitterB: number;
}

interface GlyphParticle {
  lx: number; ly: number; lz: number; // Target position
  ax: number; ay: number; az: number; // Scatter start position
  x: number; y: number; z: number;
  vx: number; vy: number; vz: number;
  baseRadius: number; baseBrightness: number;
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

const orbitNodes = ref(features.map((_, index) => ({
  baseAngle: (index / features.length) * Math.PI * 2,
  expandProgress: 0, x: 0, y: 0
})))

// === Canvas Globals ===
let animationId: number | null = null
let ctx: CanvasRenderingContext2D | null = null
let width = 0; let height = 0; let dpr = 1; let frameCount = 0

// Camera & Mouse (Rx based on 0 for absolute vertical centering)
let orbitRotation = 0
let currentMouseYaw = 0; let currentMousePitch = 0
let targetMouseYaw = 0; let targetMousePitch = 0

const ambientParticles: AmbientParticle[] = []
const glyphParticles: GlyphParticle[] = []

// Theme Colors
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

/**
 * Scan logic to create particle targets.
 * Optimized: Using Monospace font ensures identical width for Z, Y, and X.
 */
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
  for (let y = 0; y < ch; y += 6) {
    for (let x = 0; x < cw; x += 6) {
      if (data[(y * cw + x) * 4 + 3] > 80) {
        points.push({ x, y })
        if (x < minX) minX = x;
        if (x > maxX) maxX = x;
        if (y < minY) minY = y;
        if (y > maxY) maxY = y;
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

  const ambientCount = isMobile.value ? 600 : 1800
  const glyphCount = isMobile.value ? 400 : 1000

  // 1. Initialize Axis Particles
  for (let i = 0; i < ambientCount; i++) {
    const axisIndex = Math.floor(Math.random() * 3) as 0|1|2
    const axisPos = (Math.random() * 2 - 1) * AXIS_LENGTH * 1.5
    const spread = Math.pow(Math.random(), 0.8) * 200 + 10
    const angle = Math.random() * Math.PI * 2
    const jitterA = Math.cos(angle) * spread
    const jitterB = Math.sin(angle) * spread
    const brightness = clamp(1 - spread / 250, 0.1, 1)

    ambientParticles.push({
      x: 0, y: 0, z: 0,
      vx: 0, vy: 0, vz: 0,
      baseRadius: Math.random() * 1.2 + 0.6,
      baseBrightness: brightness,
      axisIndex, axisPos, jitterA, jitterB
    })
  }

  // 2. Initialize ZYX Glyph Particles
  const targets = createGlyphTargets(glyphCount)
  for (let i = 0; i < glyphCount; i++) {
    const glyph = targets[i] || { x: 0, y: 0, z: 0 }
    const ax = (Math.random() - 0.5) * AXIS_LENGTH * 2
    const ay = (Math.random() - 0.5) * AXIS_LENGTH * 1.2
    const az = (Math.random() - 0.5) * AXIS_LENGTH * 2

    glyphParticles.push({
      lx: glyph.x, ly: glyph.y, lz: glyph.z,
      ax, ay, az,
      x: ax, y: ay, z: az,
      vx: 0, vy: 0, vz: 0,
      baseRadius: isMobile.value ? 1.0 : 1.3,
      baseBrightness: 0.7 + Math.random() * 0.3
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

// Inline projection for maximum performance
function projectFast(x: number, y: number, z: number, cosX: number, sinX: number, cosY: number, sinY: number, cx: number, cy: number, fov: number) {
  const x1 = x * cosY - z * sinY
  const z1 = x * sinY + z * cosY
  const y2 = y * cosX - z1 * sinX
  const z2 = y * sinX + z1 * cosX
  const denom = fov + z2
  if (denom <= 20) return null
  const scale = fov / denom
  return { sx: cx + x1 * scale, sy: cy + y2 * scale, depth: z2, scale }
}

function drawAxes(cosX: number, sinX: number, cosY: number, sinY: number, cx: number, cy: number, fov: number, alpha: number) {
  if (!ctx || alpha <= 0.01) return
  const axes = [{ vec: [1, 0, 0] }, { vec: [0, 1, 0] }, { vec: [0, 0, 1] }]
  axes.forEach((axis) => {
    const start = projectFast(-axis.vec[0] * AXIS_LENGTH, -axis.vec[1] * AXIS_LENGTH, -axis.vec[2] * AXIS_LENGTH, cosX, sinX, cosY, sinY, cx, cy, fov)
    const end = projectFast(axis.vec[0] * AXIS_LENGTH, axis.vec[1] * AXIS_LENGTH, axis.vec[2] * AXIS_LENGTH, cosX, sinX, cosY, sinY, cx, cy, fov)
    if (!start || !end) return
    const grad = ctx.createLinearGradient(start.sx, start.sy, end.sx, end.sy)
    grad.addColorStop(0, AXIS_A); grad.addColorStop(0.5, AXIS_B); grad.addColorStop(1, AXIS_A)
    ctx.strokeStyle = grad
    ctx.lineWidth = 1.2
    ctx.globalAlpha = alpha * 0.5
    ctx.beginPath(); ctx.moveTo(start.sx, start.sy); ctx.lineTo(end.sx, end.sy); ctx.stroke()
  })
  ctx.globalAlpha = 1
}

function updateOrbitNodes() {
  if (isMobile.value) return
  if (hoveredNodeIndex.value === null) orbitRotation += 0.0003
  const cx = width * 0.5, cy = height * 0.5
  const rX = width * 0.38, rY = height * 0.38
  orbitNodes.value.forEach((node, i) => {
    const angle = node.baseAngle + orbitRotation
    node.x = clamp(cx + Math.cos(angle) * rX, 150, width - 150)
    node.y = clamp(cy + Math.sin(angle) * rY, 150, height - 150)
    const target = hoveredNodeIndex.value === i ? 1 : 0
    node.expandProgress += (target - node.expandProgress) * 0.15
  })
}

function animate() {
  if (!ctx) return
  frameCount += 1
  ctx.fillStyle = BG
  ctx.fillRect(0, 0, width, height)

  currentMouseYaw += (targetMouseYaw - currentMouseYaw) * 0.05
  currentMousePitch += (targetMousePitch - currentMousePitch) * 0.05

  const rx = currentMousePitch
  const ry = BASE_YAW + currentMouseYaw
  const cx = width * 0.5, cy = height * 0.5
  const fov = 1000

  const cosX = Math.cos(rx), sinX = Math.sin(rx)
  const cosY = Math.cos(ry), sinY = Math.sin(ry)

  const t = (frameCount % MORPH_CYCLE) / MORPH_CYCLE
  const gather = smoothstep(0.40, 0.55, t)
  const release = smoothstep(0.75, 0.90, t)
  const glyphMorph = clamp(gather - release, 0, 1)
  const axisAlpha = smoothstep(0.1, 0.5, 1 - glyphMorph)

  // 1. Ambient Particles
  for (let i = 0; i < ambientParticles.length; i++) {
    const p = ambientParticles[i]
    const wobble = 1 + Math.sin(frameCount * 0.005 + p.axisPos * 0.01) * 0.05
    let tx = 0, ty = 0, tz = 0
    if (p.axisIndex === 0) { tx = p.axisPos; ty = p.jitterA * wobble; tz = p.jitterB * wobble }
    else if (p.axisIndex === 1) { tx = p.jitterA * wobble; ty = p.axisPos; tz = p.jitterB * wobble }
    else { tx = p.jitterA * wobble; ty = p.jitterB * wobble; tz = p.axisPos }

    p.x += (tx - p.x) * 0.05
    p.y += (ty - p.y) * 0.05
    p.z += (tz - p.z) * 0.05

    const proj = projectFast(p.x, p.y, p.z, cosX, sinX, cosY, sinY, cx, cy, fov)
    if (!proj) continue

    const r = ~~(FAR_RGB[0] + (NEAR_RGB[0] - FAR_RGB[0]) * p.baseBrightness)
    const g = ~~(FAR_RGB[1] + (NEAR_RGB[1] - FAR_RGB[1]) * p.baseBrightness)
    const b = ~~(FAR_RGB[2] + (NEAR_RGB[2] - FAR_RGB[2]) * p.baseBrightness)

    const alpha = (0.2 + p.baseBrightness * 0.4) * axisAlpha
    ctx.fillStyle = `rgba(${r},${g},${b},${alpha.toFixed(2)})`
    ctx.beginPath(); ctx.arc(proj.sx, proj.sy, p.baseRadius * proj.scale, 0, Math.PI * 2); ctx.fill()
  }

  // 2. Glyph Particles
  for (let i = 0; i < glyphParticles.length; i++) {
    const p = glyphParticles[i]
    const tx = p.ax + (p.lx - p.ax) * glyphMorph
    const ty = p.ay + (p.ly - p.ay) * glyphMorph
    const tz = p.az + (p.lz - p.az) * glyphMorph

    p.x += (tx - p.x) * 0.08
    p.y += (ty - p.y) * 0.08
    p.z += (tz - p.z) * 0.08

    const proj = projectFast(p.x, p.y, p.z, cosX, sinX, cosY, sinY, cx, cy, fov)
    if (!proj) continue

    const r = ~~(FAR_RGB[0] + (NEAR_RGB[0] - FAR_RGB[0]) * p.baseBrightness)
    const g = ~~(FAR_RGB[1] + (NEAR_RGB[1] - FAR_RGB[1]) * p.baseBrightness)
    const b = ~~(FAR_RGB[2] + (NEAR_RGB[2] - FAR_RGB[2]) * p.baseBrightness)

    const alpha = 0.3 + glyphMorph * 0.6
    const radius = p.baseRadius * proj.scale * (1 + glyphMorph * 0.3)

    ctx.fillStyle = `rgba(${r},${g},${b},${alpha.toFixed(2)})`
    ctx.beginPath(); ctx.arc(proj.sx, proj.sy, radius, 0, Math.PI * 2); ctx.fill()
  }

  drawAxes(cosX, sinX, cosY, sinY, cx, cy, fov, axisAlpha)
  updateOrbitNodes()
  animationId = window.requestAnimationFrame(animate)
}

function handleMouseMove(e: MouseEvent) {
  const nx = (e.clientX / width - 0.5) * 2
  const ny = (e.clientY / height - 0.5) * 2
  targetMouseYaw = nx * 0.25
  targetMousePitch = -ny * 0.15
}

onMounted(() => {
  ctx = canvasRef.value?.getContext('2d', { alpha: false })
  if (!ctx) return
  resizeCanvas()
  window.addEventListener('resize', resizeCanvas)
  window.addEventListener('mousemove', handleMouseMove)
  window.addEventListener('mouseleave', () => { targetMouseYaw = 0; targetMousePitch = 0 })
  animationId = window.requestAnimationFrame(animate)
})

onUnmounted(() => {
  if (animationId) window.cancelAnimationFrame(animationId)
  window.removeEventListener('resize', resizeCanvas)
  window.removeEventListener('mousemove', handleMouseMove)
})
</script>

<template>
  <div class="home-root">
    <canvas ref="canvasRef" class="bg-canvas"></canvas>

    <div class="overlay-ui">
      <main class="hero-center">
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
      </main>

      <!-- Surround Features -->
      <div v-if="!isMobile" class="orbit-layer">
        <div v-for="(feature, index) in features" :key="feature.titleEn"
             class="orbit-node"
             :style="{ left: `${orbitNodes[index].x}px`, top: `${orbitNodes[index].y}px` }"
             @mouseenter="hoveredNodeIndex = index"
             @mouseleave="hoveredNodeIndex = null">
          <span class="node-dot" :class="{ 'is-active': hoveredNodeIndex === index }"></span>
          <span class="node-label">{{ isEn ? feature.titleEn : feature.titleZh }}</span>

          <div class="node-card"
               :style="{
                 opacity: orbitNodes[index].expandProgress,
                 transform: `translate(-50%, ${(1 - orbitNodes[index].expandProgress) * 12}px) scale(${0.96 + orbitNodes[index].expandProgress * 0.04})`,
                 pointerEvents: orbitNodes[index].expandProgress > 0.5 ? 'auto' : 'none'
               }">
            <div class="node-card-icon">{{ feature.icon }}</div>
            <h3>{{ isEn ? feature.titleEn : feature.titleZh }}</h3>
            <p>{{ isEn ? feature.descEn : feature.descZh }}</p>
          </div>
        </div>
      </div>

      <div v-else class="mobile-features-row">
        <div v-for="feature in features" :key="feature.titleEn" class="mobile-card">
          <div class="mobile-icon">{{ feature.icon }}</div>
          <h3>{{ isEn ? feature.titleEn : feature.titleZh }}</h3>
          <p>{{ isEn ? feature.descEn : feature.descZh }}</p>
        </div>
      </div>

      <footer class="home-footer">
        MIT License &copy; 2025 ZYX Contributors
      </footer>
    </div>
  </div>
</template>

<style scoped>
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

.hero-center {
  position: absolute;
  top: 50%;
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
}

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
  transform: translateY(-2px);
}

.btn-secondary {
  color: #b8c7d6;
  border: 1px solid rgba(122, 144, 170, 0.3);
  background: rgba(24, 34, 44, 0.4);
}

.btn-secondary:hover {
  background: rgba(118, 142, 167, 0.2);
  color: #fff;
  transform: translateY(-2px);
}

.orbit-layer {
  position: absolute;
  inset: 0;
  pointer-events: none;
}

.orbit-node {
  position: absolute;
  transform: translate(-50%, -50%);
  display: flex;
  align-items: center;
  gap: 0.5rem;
  color: #7a8d9f;
  pointer-events: auto;
  cursor: pointer;
}

.node-dot {
  width: 7px; height: 7px;
  border-radius: 50%;
  background: currentColor;
  transition: all 0.3s ease;
}

.node-dot.is-active {
  transform: scale(1.6);
  background: #e7edf5;
}

.node-label {
  font-size: 0.7rem;
  letter-spacing: 0.08em;
  text-transform: uppercase;
}

.node-card {
  position: absolute;
  top: 1.6rem; left: 50%;
  width: 250px;
  padding: 1.1rem;
  background: rgba(15, 22, 30, 0.85);
  backdrop-filter: blur(12px);
  border: 1px solid rgba(122, 144, 170, 0.25);
  border-radius: 12px;
  box-shadow: 0 20px 40px rgba(0,0,0,0.5);
  transition: opacity 0.3s ease, transform 0.3s ease;
  text-align: center;
}

.node-card-icon {
  display: inline-flex;
  padding: 0.25rem 0.65rem;
  border-radius: 50px;
  background: rgba(122, 144, 170, 0.15);
  color: #c5d2de;
  font-size: 0.65rem;
  font-weight: 600;
  margin-bottom: 0.7rem;
}

.node-card h3 {
  margin: 0 0 0.4rem;
  font-size: 0.9rem;
  color: #e7edf5;
}

.node-card p {
  margin: 0;
  font-size: 0.75rem;
  line-height: 1.5;
  color: #8a9bb0;
}

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

.home-footer {
  position: absolute;
  bottom: 0.8rem;
  left: 50%;
  transform: translateX(-50%);
  font-size: 0.7rem;
  color: #566b82;
}

@media (max-width: 768px) {
  .hero-center { top: 40%; width: calc(100vw - 2.4rem); }
  .hero-title { font-size: 1.6rem; }
  .hero-actions { flex-direction: column; width: 100%; }
  .btn-primary, .btn-secondary { width: 100%; }
}
</style>