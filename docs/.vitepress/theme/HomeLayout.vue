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

interface AmbientParticle {
  x: number
  y: number
  z: number
  vx: number
  vy: number
  vz: number
  baseRadius: number
  baseBrightness: number
  axisIndex: 0 | 1 | 2
  axisPos: number
  jitterA: number
  jitterB: number
}

interface GlyphParticle {
  lx: number
  ly: number
  lz: number
  ax: number
  ay: number
  az: number
  x: number
  y: number
  z: number
  vx: number
  vy: number
  vz: number
  baseRadius: number
  baseBrightness: number
}

interface FeatureData {
  icon: string
  titleEn: string
  titleZh: string
  descEn: string
  descZh: string
  axis: AxisDirection
}

interface OrbitNodeState {
  baseAngle: number
  expandProgress: number
  x: number
  y: number
}

interface AxisLabelState {
  x: number
  y: number
  opacity: number
}

const axisLabelState = ref<Record<AxisLetter, AxisLabelState>>({
  X: { x: 0, y: 0, opacity: 0 },
  Y: { x: 0, y: 0, opacity: 0 },
  Z: { x: 0, y: 0, opacity: 0 }
})

const features: FeatureData[] = [
  {
    icon: 'IO',
    titleEn: 'Throughput Engine',
    titleZh: '吞吐引擎',
    descEn: 'Segmented storage and parallel IO keep heavy graph workloads stable.',
    descZh: '分段存储与并行 IO 让高负载图查询保持稳定吞吐。',
    axis: 'x+'
  },
  {
    icon: 'TX',
    titleEn: 'Transactional Core',
    titleZh: '事务核心',
    descEn: 'WAL + optimistic control + rollback keep state transitions safe.',
    descZh: 'WAL、乐观并发与回滚机制共同保障状态转换安全。',
    axis: 'x-'
  },
  {
    icon: 'CY',
    titleEn: 'Cypher Native',
    titleZh: 'Cypher 原生',
    descEn: 'Parser, planner, and optimizer tuned for graph-first semantics.',
    descZh: '解析、规划、优化链路面向图语义原生设计。',
    axis: 'y+'
  },
  {
    icon: 'VS',
    titleEn: 'Vector Path',
    titleZh: '向量路径',
    descEn: 'DiskANN and quantization integrate ANN into transactional graphs.',
    descZh: 'DiskANN 与量化流程将 ANN 能力融入事务型图系统。',
    axis: 'y-'
  },
  {
    icon: 'EM',
    titleEn: 'Embeddable API',
    titleZh: '可嵌入 API',
    descEn: 'Lean C++/C interfaces with no external daemon requirement.',
    descZh: '轻量 C++/C 接口，无需外部守护进程即可集成。',
    axis: 'z+'
  },
  {
    icon: 'OS',
    titleEn: 'Open Source',
    titleZh: '开源协作',
    descEn: 'MIT-licensed codebase designed for transparent evolution.',
    descZh: 'MIT 许可，演进路径透明，便于长期协作与维护。',
    axis: 'z-'
  }
]

const orbitNodes = ref<OrbitNodeState[]>(
  features.map((_, index) => ({
    baseAngle: (index / features.length) * Math.PI * 2,
    expandProgress: 0,
    x: 0,
    y: 0
  }))
)

let animationId: number | null = null
let resizeHandler: (() => void) | null = null
let ctx: CanvasRenderingContext2D | null = null
let width = 0
let height = 0
let dpr = 1
let frameCount = 0
let orbitRotation = 0
let currentMouseYaw = 0
let currentMousePitch = 0
let targetMouseYaw = 0
let targetMousePitch = 0
let pointerX = 0
let pointerY = 0
let pointerActive = false

const ambientParticles: AmbientParticle[] = []
const glyphParticles: GlyphParticle[] = []

const axisPulse: Record<AxisDirection, number> = {
  'x+': 0,
  'x-': 0,
  'y+': 0,
  'y-': 0,
  'z+': 0,
  'z-': 0
}

const BG = '#0b0f14'
const AXIS_A = '#5f748b'
const AXIS_B = '#9db2c7'
const FAR_PARTICLE_RGB: [number, number, number] = [30, 40, 52]
const NEAR_PARTICLE_RGB: [number, number, number] = [148, 168, 190]

const AXIS_LENGTH = 420
const MORPH_CYCLE = 2400
const BASE_YAW = -0.52

const clamp = (value: number, min: number, max: number) => Math.max(min, Math.min(max, value))
const lerp = (a: number, b: number, t: number) => a + (b - a) * t

function smoothstep(edge0: number, edge1: number, x: number) {
  const t = clamp((x - edge0) / (edge1 - edge0), 0, 1)
  return t * t * (3 - 2 * t)
}

function getAmbientCount() {
  return isMobile.value ? 620 : 2200
}

function getGlyphCount() {
  return isMobile.value ? 260 : 840
}

function updateMobileFlag() {
  isMobile.value = window.innerWidth <= 768
}

function randomSpherePoint(maxRadius = 400) {
  const u = Math.random()
  const v = Math.random()
  const w = Math.random()

  const theta = Math.acos(2 * u - 1)
  const phi = 2 * Math.PI * v
  const radius = maxRadius * Math.cbrt(w)

  const sinTheta = Math.sin(theta)

  return {
    x: radius * sinTheta * Math.cos(phi),
    y: radius * Math.cos(theta),
    z: radius * sinTheta * Math.sin(phi)
  }
}

function computeBaseBrightness(x: number, y: number, z: number) {
  const distToX = Math.sqrt(y * y + z * z)
  const distToY = Math.sqrt(x * x + z * z)
  const distToZ = Math.sqrt(x * x + y * y)
  const nearest = Math.min(distToX, distToY, distToZ)
  return clamp(1 - nearest / 220, 0, 1)
}

function shuffleInPlace<T>(items: T[]) {
  for (let i = items.length - 1; i > 0; i--) {
    const j = Math.floor(Math.random() * (i + 1))
    const tmp = items[i]
    items[i] = items[j]
    items[j] = tmp
  }
}

function createGlyphTargets(count: number) {
  const canvas = document.createElement('canvas')
  const cw = isMobile.value ? 1300 : 2300
  const ch = isMobile.value ? 820 : 1280
  canvas.width = cw
  canvas.height = ch

  const textCtx = canvas.getContext('2d')
  if (!textCtx) {
    return []
  }

  textCtx.clearRect(0, 0, cw, ch)
  textCtx.fillStyle = '#ffffff'
  textCtx.textAlign = 'center'
  textCtx.textBaseline = 'middle'
  textCtx.font = `${isMobile.value ? 380 : 700}px "Avenir Next", "Segoe UI", sans-serif`
  textCtx.fillText('ZYX', cw * 0.5, ch * 0.56)

  const data = textCtx.getImageData(0, 0, cw, ch).data
  const points: Array<{ x: number; y: number }> = []
  const step = isMobile.value ? 6 : 5

  for (let y = 0; y < ch; y += step) {
    for (let x = 0; x < cw; x += step) {
      const alpha = data[(y * cw + x) * 4 + 3]
      if (alpha > 80) {
        points.push({ x, y })
      }
    }
  }

  if (points.length === 0) {
    return []
  }

  shuffleInPlace(points)
  const worldWidth = Math.min(2100, Math.max(1200, width * 1.42))
  const worldHeight = Math.min(1220, Math.max(620, height * 1.18))
  const scaleX = worldWidth / cw
  const scaleY = worldHeight / ch
  const zBase = isMobile.value ? -170 : -230
  const yOffset = isMobile.value ? -14 : -38
  const targets: Array<{ x: number; y: number; z: number }> = []

  for (let i = 0; i < count; i++) {
    const point = points[i % points.length]
    targets.push({
      x: (point.x - cw * 0.5) * scaleX,
      y: (ch * 0.54 - point.y) * scaleY + yOffset,
      z: zBase + (Math.random() - 0.5) * 72
    })
  }

  return targets
}

function getAmbientTarget(particle: AmbientParticle) {
  const wobble = 1 + Math.sin(frameCount * 0.0024 + particle.axisPos * 0.012) * 0.08

  if (particle.axisIndex === 0) {
    return {
      x: particle.axisPos,
      y: particle.jitterA * wobble,
      z: particle.jitterB * wobble
    }
  }

  if (particle.axisIndex === 1) {
    return {
      x: particle.jitterA * wobble,
      y: particle.axisPos,
      z: particle.jitterB * wobble
    }
  }

  return {
    x: particle.jitterA * wobble,
    y: particle.jitterB * wobble,
    z: particle.axisPos
  }
}

function resetAmbientParticles() {
  ambientParticles.length = 0
  const count = getAmbientCount()

  for (let i = 0; i < count; i++) {
    const axisRand = Math.random()
    const axisIndex: 0 | 1 | 2 = axisRand < 1 / 3 ? 0 : axisRand < 2 / 3 ? 1 : 2
    const axisPos = (Math.random() * 2 - 1) * AXIS_LENGTH * 1.05
    const spread = Math.pow(Math.random(), 0.64) * 120 + 8
    const angle = Math.random() * Math.PI * 2
    const jitterA = Math.cos(angle) * spread
    const jitterB = Math.sin(angle) * spread

    const seed = getAmbientTarget({
      x: 0,
      y: 0,
      z: 0,
      vx: 0,
      vy: 0,
      vz: 0,
      baseRadius: 0,
      baseBrightness: 0,
      axisIndex,
      axisPos,
      jitterA,
      jitterB
    })

    const brightness = clamp(0.2 + (1 - spread / 140) * 0.8, 0.12, 1)

    ambientParticles.push({
      x: seed.x + (Math.random() - 0.5) * 6,
      y: seed.y + (Math.random() - 0.5) * 6,
      z: seed.z + (Math.random() - 0.5) * 6,
      vx: (Math.random() - 0.5) * 0.06,
      vy: (Math.random() - 0.5) * 0.06,
      vz: (Math.random() - 0.5) * 0.06,
      baseRadius: 0.48 + brightness * 1.45,
      baseBrightness: brightness,
      axisIndex,
      axisPos,
      jitterA,
      jitterB
    })
  }
}

function resetGlyphParticles() {
  glyphParticles.length = 0
  const count = getGlyphCount()
  const glyphTargets = createGlyphTargets(count)

  if (glyphTargets.length === 0) {
    for (let i = 0; i < count; i++) {
      const pos = randomSpherePoint(180)
      glyphParticles.push({
        lx: pos.x,
        ly: pos.y,
        lz: pos.z,
        ax: pos.x * 0.2,
        ay: pos.y * 0.2,
        az: pos.z * 0.2,
        x: pos.x,
        y: pos.y,
        z: pos.z,
        vx: (Math.random() - 0.5) * 0.06,
        vy: (Math.random() - 0.5) * 0.06,
        vz: (Math.random() - 0.5) * 0.06,
        baseRadius: 1.2,
        baseBrightness: 0.75
      })
    }
    return
  }

  for (let i = 0; i < count; i++) {
    const glyph = glyphTargets[i]
    const axisIndex: 0 | 1 | 2 = (i % 3) as 0 | 1 | 2
    const axisPos = (Math.random() * 2 - 1) * AXIS_LENGTH * 0.92
    const jitterA = (Math.random() * 2 - 1) * 16
    const jitterB = (Math.random() * 2 - 1) * 16

    let ax = 0
    let ay = 0
    let az = 0

    if (axisIndex === 0) {
      ax = axisPos
      ay = jitterA
      az = jitterB
    } else if (axisIndex === 1) {
      ax = jitterA
      ay = axisPos
      az = jitterB
    } else {
      ax = jitterA
      ay = jitterB
      az = axisPos
    }

    glyphParticles.push({
      lx: glyph.x,
      ly: glyph.y,
      lz: glyph.z,
      ax,
      ay,
      az,
      x: glyph.x + (Math.random() - 0.5) * 6,
      y: glyph.y + (Math.random() - 0.5) * 6,
      z: glyph.z + (Math.random() - 0.5) * 6,
      vx: (Math.random() - 0.5) * 0.05,
      vy: (Math.random() - 0.5) * 0.05,
      vz: (Math.random() - 0.5) * 0.05,
      baseRadius: isMobile.value ? 1.05 : 1.25,
      baseBrightness: 0.72 + Math.random() * 0.25
    })
  }
}

function resetParticleSystems() {
  resetAmbientParticles()
  resetGlyphParticles()
}

function resizeCanvas(forceParticleReset = false) {
  const canvas = canvasRef.value
  if (!canvas || !ctx) return

  width = window.innerWidth
  height = window.innerHeight
  dpr = Math.min(window.devicePixelRatio || 1, 2)

  canvas.width = Math.floor(width * dpr)
  canvas.height = Math.floor(height * dpr)
  canvas.style.width = `${width}px`
  canvas.style.height = `${height}px`

  ctx.setTransform(dpr, 0, 0, dpr, 0, 0)

  const previousMobile = isMobile.value
  updateMobileFlag()

  const ambientCount = getAmbientCount()
  const glyphCount = getGlyphCount()
  if (
    forceParticleReset ||
    previousMobile !== isMobile.value ||
    ambientParticles.length !== ambientCount ||
    glyphParticles.length !== glyphCount
  ) {
    resetParticleSystems()
  }

  pointerX = width * 0.5
  pointerY = height * 0.5
}

function getGlyphMorphProgress() {
  const t = (frameCount % MORPH_CYCLE) / MORPH_CYCLE

  const gather = smoothstep(0.44, 0.58, t)
  const release = smoothstep(0.68, 0.82, t)
  return clamp(gather - release, 0, 1)
}

function projectPoint(
  x: number,
  y: number,
  z: number,
  rx: number,
  ry: number,
  cx: number,
  cy: number,
  perspective: number
) {
  const cosY = Math.cos(ry)
  const sinY = Math.sin(ry)
  const cosX = Math.cos(rx)
  const sinX = Math.sin(rx)

  const xAfterY = x * cosY - z * sinY
  const zAfterY = x * sinY + z * cosY

  const yAfterX = y * cosX - zAfterY * sinX
  const zAfterX = y * sinX + zAfterY * cosX

  const denom = perspective + zAfterX
  if (denom <= 20) return null

  const scale = perspective / denom

  return {
    sx: cx + xAfterY * scale,
    sy: cy + yAfterX * scale,
    depth: zAfterX,
    scale
  }
}

function mixParticleColor(brightness: number) {
  const t = clamp(brightness, 0, 1)
  const r = FAR_PARTICLE_RGB[0] + (NEAR_PARTICLE_RGB[0] - FAR_PARTICLE_RGB[0]) * t
  const g = FAR_PARTICLE_RGB[1] + (NEAR_PARTICLE_RGB[1] - FAR_PARTICLE_RGB[1]) * t
  const b = FAR_PARTICLE_RGB[2] + (NEAR_PARTICLE_RGB[2] - FAR_PARTICLE_RGB[2]) * t
  return [r, g, b]
}

function axisLabelStyle(letter: AxisLetter) {
  const state = axisLabelState.value[letter]

  return {
    left: `${state.x}px`,
    top: `${state.y}px`,
    opacity: state.opacity,
    transform: `translate(-50%, -50%) scale(${0.86 + state.opacity * 0.14})`
  }
}

function updateAxisLabels(rx: number, ry: number, cx: number, cy: number, perspective: number, axisOpacity: number) {
  const points: Record<AxisLetter, { x: number; y: number; z: number }> = {
    X: { x: AXIS_LENGTH + 64, y: 0, z: 0 },
    Y: { x: 0, y: AXIS_LENGTH + 64, z: 0 },
    Z: { x: 0, y: 0, z: AXIS_LENGTH + 64 }
  }

  ;(['X', 'Y', 'Z'] as AxisLetter[]).forEach((letter) => {
    const p = points[letter]
    const projected = projectPoint(p.x, p.y, p.z, rx, ry, cx, cy, perspective)

    if (projected) {
      axisLabelState.value[letter].x = projected.sx
      axisLabelState.value[letter].y = projected.sy
      axisLabelState.value[letter].opacity = axisOpacity
    } else {
      axisLabelState.value[letter].opacity = 0
    }
  })
}

function updateOrbitNodes(glyphMorph: number) {
  if (isMobile.value) return

  if (hoveredNodeIndex.value === null) {
    orbitRotation += 0.00022 * (1 - glyphMorph * 0.28)
  }

  const centerX = width * 0.5
  const centerY = height * 0.82
  const radiusX = Math.min(width * 0.33, 560)
  const radiusY = Math.min(height * 0.095, 120)

  orbitNodes.value.forEach((node, index) => {
    const angle = node.baseAngle + orbitRotation
    node.x = clamp(centerX + Math.cos(angle) * radiusX, 120, width - 120)
    node.y = clamp(centerY + Math.sin(angle) * radiusY, 120, height - 120)

    const target = hoveredNodeIndex.value === index ? 1 : 0
    node.expandProgress += (target - node.expandProgress) * 0.16
  })
}

function drawAxes(rx: number, ry: number, cx: number, cy: number, perspective: number, axisOpacity: number) {
  if (!ctx) return

  const tickStep = 40
  const tickSize = 8

  const axes: Array<{
    vec: [number, number, number]
    tickVec: [number, number, number]
    posKey: AxisDirection
    negKey: AxisDirection
  }> = [
    { vec: [1, 0, 0], tickVec: [0, 1, 0], posKey: 'x+', negKey: 'x-' },
    { vec: [0, 1, 0], tickVec: [1, 0, 0], posKey: 'y+', negKey: 'y-' },
    { vec: [0, 0, 1], tickVec: [1, 0, 0], posKey: 'z+', negKey: 'z-' }
  ]

  for (const key of Object.keys(axisPulse) as AxisDirection[]) {
    axisPulse[key] *= 0.93
  }

  axes.forEach((axisData) => {
    const start = projectPoint(
      -axisData.vec[0] * AXIS_LENGTH,
      -axisData.vec[1] * AXIS_LENGTH,
      -axisData.vec[2] * AXIS_LENGTH,
      rx,
      ry,
      cx,
      cy,
      perspective
    )

    const end = projectPoint(
      axisData.vec[0] * AXIS_LENGTH,
      axisData.vec[1] * AXIS_LENGTH,
      axisData.vec[2] * AXIS_LENGTH,
      rx,
      ry,
      cx,
      cy,
      perspective
    )

    if (!start || !end) return

    const gradient = ctx.createLinearGradient(start.sx, start.sy, end.sx, end.sy)
    gradient.addColorStop(0, AXIS_A)
    gradient.addColorStop(0.5, AXIS_B)
    gradient.addColorStop(1, AXIS_A)

    ctx.strokeStyle = gradient
    ctx.lineWidth = 1.15
    ctx.globalAlpha = axisOpacity
    ctx.beginPath()
    ctx.moveTo(start.sx, start.sy)
    ctx.lineTo(end.sx, end.sy)
    ctx.stroke()

    for (let t = -AXIS_LENGTH + tickStep; t < AXIS_LENGTH; t += tickStep) {
      if (Math.abs(t) < 10) continue

      const baseX = axisData.vec[0] * t
      const baseY = axisData.vec[1] * t
      const baseZ = axisData.vec[2] * t

      const tickA = projectPoint(
        baseX - axisData.tickVec[0] * tickSize,
        baseY - axisData.tickVec[1] * tickSize,
        baseZ - axisData.tickVec[2] * tickSize,
        rx,
        ry,
        cx,
        cy,
        perspective
      )

      const tickB = projectPoint(
        baseX + axisData.tickVec[0] * tickSize,
        baseY + axisData.tickVec[1] * tickSize,
        baseZ + axisData.tickVec[2] * tickSize,
        rx,
        ry,
        cx,
        cy,
        perspective
      )

      if (!tickA || !tickB) continue

      ctx.strokeStyle = 'rgba(157, 178, 200, 0.58)'
      ctx.lineWidth = 0.85
      ctx.globalAlpha = axisOpacity * 0.92
      ctx.beginPath()
      ctx.moveTo(tickA.sx, tickA.sy)
      ctx.lineTo(tickB.sx, tickB.sy)
      ctx.stroke()
    }

    const posPulse = axisPulse[axisData.posKey]
    const negPulse = axisPulse[axisData.negKey]

    if (posPulse > 0.02) {
      const glow = ctx.createRadialGradient(end.sx, end.sy, 0, end.sx, end.sy, 32 * posPulse + 10)
      glow.addColorStop(0, `rgba(157, 178, 200, ${0.55 * posPulse + 0.2})`)
      glow.addColorStop(1, 'rgba(157, 178, 200, 0)')

      ctx.globalAlpha = axisOpacity
      ctx.fillStyle = glow
      ctx.beginPath()
      ctx.arc(end.sx, end.sy, 32 * posPulse + 10, 0, Math.PI * 2)
      ctx.fill()
    }

    if (negPulse > 0.02) {
      const glow = ctx.createRadialGradient(start.sx, start.sy, 0, start.sx, start.sy, 32 * negPulse + 10)
      glow.addColorStop(0, `rgba(157, 178, 200, ${0.55 * negPulse + 0.2})`)
      glow.addColorStop(1, 'rgba(157, 178, 200, 0)')

      ctx.globalAlpha = axisOpacity
      ctx.fillStyle = glow
      ctx.beginPath()
      ctx.arc(start.sx, start.sy, 32 * negPulse + 10, 0, Math.PI * 2)
      ctx.fill()
    }
  })

  ctx.globalAlpha = 1
}

function animate() {
  if (!ctx) return

  frameCount += 1

  const glyphMorph = getGlyphMorphProgress()
  const axisVisibility = smoothstep(0.18, 0.62, glyphMorph)
  const axisOpacity = axisVisibility * 0.96

  ctx.fillStyle = BG
  ctx.fillRect(0, 0, width, height)

  currentMouseYaw += (targetMouseYaw - currentMouseYaw) * 0.05
  currentMousePitch += (targetMousePitch - currentMousePitch) * 0.05

  const rx = 0.4 + currentMousePitch
  const ry = BASE_YAW + currentMouseYaw
  const cx = width * 0.5
  const cy = height * 0.5
  const perspective = 790

  const projectedParticles: Array<{
    sx: number
    sy: number
    depth: number
    radius: number
    brightness: number
    alpha: number
    glyph: boolean
  }> = []

  for (const particle of ambientParticles) {
    particle.vx = clamp(particle.vx + (Math.random() - 0.5) * 0.002, -0.12, 0.12)
    particle.vy = clamp(particle.vy + (Math.random() - 0.5) * 0.002, -0.12, 0.12)
    particle.vz = clamp(particle.vz + (Math.random() - 0.5) * 0.002, -0.12, 0.12)

    const axisTarget = getAmbientTarget(particle)
    const targetX = axisTarget.x
    const targetY = axisTarget.y
    const targetZ = axisTarget.z

    const spring = pointerActive ? 0.014 : 0.022

    particle.x += particle.vx + (targetX - particle.x) * spring
    particle.y += particle.vy + (targetY - particle.y) * spring
    particle.z += particle.vz + (targetZ - particle.z) * spring

    const projected = projectPoint(particle.x, particle.y, particle.z, rx, ry, cx, cy, perspective)
    if (!projected || projected.scale <= 0) continue

    let boost = 1

    if (pointerActive) {
      const dx = pointerX - projected.sx
      const dy = pointerY - projected.sy
      const distance = Math.sqrt(dx * dx + dy * dy)

      if (distance < 150) {
        const normalized = 1 - distance / 150
        boost = 1 + normalized * 2

        const attraction = normalized * normalized * 0.01
        particle.x += dx * attraction
        particle.y += dy * attraction
      }
    }

    projectedParticles.push({
      sx: projected.sx,
      sy: projected.sy,
      depth: projected.depth,
      radius: Math.max(0.22, particle.baseRadius * projected.scale * (0.82 + boost * 0.2)),
      brightness: clamp(particle.baseBrightness * boost, 0, 1),
      alpha: 0.56 + particle.baseBrightness * 0.16,
      glyph: false
    })
  }

  for (const particle of glyphParticles) {
    particle.vx = clamp(particle.vx + (Math.random() - 0.5) * 0.0016, -0.09, 0.09)
    particle.vy = clamp(particle.vy + (Math.random() - 0.5) * 0.0016, -0.09, 0.09)
    particle.vz = clamp(particle.vz + (Math.random() - 0.5) * 0.0016, -0.09, 0.09)

    const targetX = lerp(particle.lx, particle.ax, glyphMorph)
    const targetY = lerp(particle.ly, particle.ay, glyphMorph)
    const targetZ = lerp(particle.lz, particle.az, glyphMorph)

    const spring = 0.032 + glyphMorph * 0.05
    particle.x += particle.vx + (targetX - particle.x) * spring
    particle.y += particle.vy + (targetY - particle.y) * spring
    particle.z += particle.vz + (targetZ - particle.z) * spring

    const projected = projectPoint(particle.x, particle.y, particle.z, rx, ry, cx, cy, perspective)
    if (!projected || projected.scale <= 0) continue

    let boost = 1
    if (pointerActive) {
      const dx = pointerX - projected.sx
      const dy = pointerY - projected.sy
      const distance = Math.sqrt(dx * dx + dy * dy)
      if (distance < 150) {
        const normalized = 1 - distance / 150
        boost = 1 + normalized * 2
      }
    }

    projectedParticles.push({
      sx: projected.sx,
      sy: projected.sy,
      depth: projected.depth,
      radius: Math.max(0.28, particle.baseRadius * projected.scale * (0.94 + boost * 0.18)),
      brightness: clamp(particle.baseBrightness * boost, 0, 1),
      alpha: 0.7 + (1 - glyphMorph) * 0.25,
      glyph: true
    })
  }

  projectedParticles.sort((a, b) => b.depth - a.depth)

  for (const p of projectedParticles) {
    const [r, g, b] = mixParticleColor(p.brightness * (p.glyph ? 1.16 : 1))
    const alpha = (p.glyph ? 0.22 : 0.12) + p.brightness * (p.glyph ? 0.72 : 0.58) * p.alpha

    ctx.fillStyle = `rgba(${r.toFixed(0)}, ${g.toFixed(0)}, ${b.toFixed(0)}, ${alpha.toFixed(3)})`
    ctx.beginPath()
    ctx.arc(p.sx, p.sy, p.radius, 0, Math.PI * 2)
    ctx.fill()
  }

  if (axisOpacity > 0.01) {
    drawAxes(rx, ry, cx, cy, perspective, axisOpacity)
  }
  updateAxisLabels(rx, ry, cx, cy, perspective, axisVisibility)
  updateOrbitNodes(glyphMorph)

  animationId = window.requestAnimationFrame(animate)
}

function handleMouseMove(event: MouseEvent) {
  pointerActive = true
  pointerX = event.clientX
  pointerY = event.clientY

  const normX = (event.clientX / Math.max(width, 1) - 0.5) * 2
  const normY = (event.clientY / Math.max(height, 1) - 0.5) * 2

  targetMouseYaw = clamp(normX, -1, 1) * (Math.PI / 9)
  targetMousePitch = clamp(-normY, -1, 1) * (Math.PI / 18)
}

function handleMouseLeave() {
  pointerActive = false
  targetMouseYaw = 0
  targetMousePitch = 0
}

function handleFeatureEnter(index: number) {
  hoveredNodeIndex.value = index
  axisPulse[features[index].axis] = 1
}

function handleFeatureLeave(index: number) {
  if (hoveredNodeIndex.value === index) {
    hoveredNodeIndex.value = null
  }
}

onMounted(() => {
  const canvas = canvasRef.value
  if (!canvas) return

  ctx = canvas.getContext('2d')
  if (!ctx) return

  resizeCanvas(true)
  updateOrbitNodes(0)

  resizeHandler = () => resizeCanvas(false)
  window.addEventListener('resize', resizeHandler)
  window.addEventListener('mousemove', handleMouseMove)
  window.addEventListener('mouseleave', handleMouseLeave)

  animationId = window.requestAnimationFrame(animate)
})

onUnmounted(() => {
  if (animationId !== null) {
    window.cancelAnimationFrame(animationId)
  }

  if (resizeHandler) {
    window.removeEventListener('resize', resizeHandler)
    resizeHandler = null
  }

  window.removeEventListener('mousemove', handleMouseMove)
  window.removeEventListener('mouseleave', handleMouseLeave)
})
</script>

<template>
  <div class="home-root">
    <canvas ref="canvasRef" class="bg-canvas"></canvas>

    <section class="hero-shell">
      <div class="hero-center">
        <div class="hero-badge">{{ isEn ? 'GRAPH DATABASE ENGINE' : '图数据库引擎' }}</div>

        <p class="hero-title" v-if="isEn">Coordinate-native graph runtime</p>
        <p class="hero-title" v-else>坐标原生的图数据库运行时</p>

        <p class="hero-subtitle" v-if="isEn">
          Axis particles stay distributed as the system baseline.<br>
          Background ZYX particles periodically converge into the coordinate frame.
        </p>
        <p class="hero-subtitle" v-else>
          坐标轴周围的粒子保持稳定分布，代表系统基态。<br>
          背景中的 ZYX 粒子字母会周期性收束到坐标框架。
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
      </div>

      <div v-if="!isMobile" class="axis-title-layer">
        <div class="axis-letter" :style="axisLabelStyle('X')">
          <strong>X</strong>
          <span>Execution</span>
        </div>
        <div class="axis-letter" :style="axisLabelStyle('Y')">
          <strong>Y</strong>
          <span>Consistency</span>
        </div>
        <div class="axis-letter" :style="axisLabelStyle('Z')">
          <strong>Z</strong>
          <span>Scalability</span>
        </div>
      </div>

      <div v-if="!isMobile" class="orbit-layer">
        <div
          v-for="(feature, index) in features"
          :key="feature.titleEn"
          class="orbit-node"
          :style="{
            left: `${orbitNodes[index].x}px`,
            top: `${orbitNodes[index].y}px`
          }"
          @mouseenter="handleFeatureEnter(index)"
          @mouseleave="handleFeatureLeave(index)"
        >
          <span class="node-dot"></span>
          <span class="node-label">{{ isEn ? feature.titleEn : feature.titleZh }}</span>

          <div
            class="node-card"
            :style="{
              opacity: orbitNodes[index].expandProgress,
              transform: `translateY(${(1 - orbitNodes[index].expandProgress) * 8}px) scale(${0.96 + orbitNodes[index].expandProgress * 0.04})`
            }"
          >
            <div class="node-card-icon">{{ feature.icon }}</div>
            <h3>{{ isEn ? feature.titleEn : feature.titleZh }}</h3>
            <p>{{ isEn ? feature.descEn : feature.descZh }}</p>
          </div>
        </div>
      </div>

      <div v-else class="mobile-features">
        <div v-for="feature in features" :key="feature.titleEn" class="mobile-feature-card">
          <div class="mobile-icon">{{ feature.icon }}</div>
          <h3>{{ isEn ? feature.titleEn : feature.titleZh }}</h3>
          <p>{{ isEn ? feature.descEn : feature.descZh }}</p>
        </div>
      </div>

      <div v-if="!isMobile" class="code-orb" tabindex="0" role="button" aria-label="Preview code snippet">
        <div class="code-orb-trigger">
          <span class="code-orb-dot"></span>
          <span class="code-orb-text">{{ isEn ? 'Quick Example' : '示例代码' }}</span>
        </div>
        <div class="code-orb-panel">
          <div class="code-orb-head">graph_session.cpp</div>
<pre><code>// deterministic core + flexible query surface
auto db = zyx::open("graph.db");
auto tx = db->beginTransaction();

tx->execute("CREATE (a:Person {name:'Alice'})");
tx->execute("CREATE (b:Person {name:'Bob'})");
tx->execute("MATCH (a:Person {name:'Alice'}), (b:Person {name:'Bob'})\\n"
            "CREATE (a)-[:KNOWS]->(b)");

auto rs = tx->query("MATCH (n:Person) RETURN n.name");
tx->commit();</code></pre>
        </div>
      </div>

      <footer class="home-footer">
        <p>Released under the MIT License.</p>
        <p>Copyright &copy; 2025-present ZYX Contributors</p>
      </footer>
    </section>
  </div>
</template>

<style scoped>
.home-root {
  position: relative;
  height: 100vh;
  overflow: hidden;
  background: #0b0f14;
  color: #e7edf5;
  font-family: 'Avenir Next', 'Segoe UI', sans-serif;
}

.home-root::before {
  content: '';
  position: absolute;
  inset: 0;
  pointer-events: none;
  background:
    radial-gradient(circle at 50% 35%, rgba(122, 144, 170, 0.12), transparent 52%),
    radial-gradient(circle at 20% 80%, rgba(86, 107, 130, 0.08), transparent 48%);
  z-index: 1;
}

.bg-canvas {
  position: fixed;
  inset: 0;
  width: 100%;
  height: 100%;
  z-index: 0;
}

.hero-shell {
  position: relative;
  z-index: 3;
  width: 100%;
  height: 100%;
}

.hero-center {
  position: absolute;
  top: 40%;
  left: 50%;
  transform: translate(-50%, -50%);
  width: min(920px, calc(100vw - 2.4rem));
  padding: 1.15rem 1.35rem 1.45rem;
  border-radius: 22px;
  border: 1px solid rgba(122, 144, 170, 0.16);
  background: linear-gradient(160deg, rgba(16, 22, 30, 0.6), rgba(16, 22, 30, 0.22));
  backdrop-filter: blur(6px);
  box-shadow: 0 20px 50px rgba(6, 10, 16, 0.34);
  text-align: center;
  display: flex;
  flex-direction: column;
  align-items: center;
  animation: hero-rise 0.8s ease-out;
}

@keyframes hero-rise {
  from {
    opacity: 0;
    transform: translate(-50%, -46%);
  }

  to {
    opacity: 1;
    transform: translate(-50%, -50%);
  }
}

.hero-badge {
  display: inline-flex;
  align-items: center;
  justify-content: center;
  padding: 0.35rem 0.95rem;
  margin-bottom: 1rem;
  border-radius: 999px;
  border: 1px solid rgba(122, 144, 170, 0.42);
  background: rgba(22, 30, 39, 0.75);
  color: #aec0d2;
  font-size: 0.7rem;
  letter-spacing: 0.14em;
  text-transform: uppercase;
  max-width: min(96vw, 560px);
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}

.hero-title {
  margin: 0;
  font-size: clamp(1.75rem, 3vw, 3rem);
  font-weight: 600;
  letter-spacing: 0.02em;
  color: #e7edf5;
  max-width: min(96vw, 820px);
  overflow-wrap: anywhere;
}

.hero-subtitle {
  margin: 0.8rem 0 1.45rem;
  color: #9db0c4;
  font-size: clamp(0.86rem, 1.2vw, 1rem);
  line-height: 1.72;
  max-width: min(96vw, 760px);
  overflow-wrap: anywhere;
}

.hero-actions {
  display: flex;
  gap: 0.7rem;
  align-items: center;
  flex-wrap: wrap;
  justify-content: center;
}

.btn-primary,
.btn-secondary {
  display: inline-flex;
  align-items: center;
  justify-content: center;
  gap: 0.45rem;
  min-width: 138px;
  padding: 0.62rem 1.1rem;
  border-radius: 9px;
  text-decoration: none;
  font-size: 0.82rem;
  letter-spacing: 0.05em;
  transition: all 0.24s ease;
}

.btn-primary {
  color: #e7edf5;
  border: 1px solid rgba(122, 144, 170, 0.82);
  background: rgba(29, 40, 52, 0.48);
}

.btn-primary:hover {
  background: #778ea7;
  color: #0f151c;
}

.btn-secondary {
  color: #b8c7d6;
  border: 1px solid rgba(122, 144, 170, 0.4);
  background: rgba(24, 34, 44, 0.32);
}

.btn-secondary:hover {
  border-color: rgba(122, 144, 170, 0.82);
  background: rgba(118, 142, 167, 0.2);
}

.axis-title-layer {
  position: absolute;
  inset: 0;
  z-index: 4;
  pointer-events: none;
}

.axis-letter {
  position: absolute;
  display: flex;
  flex-direction: column;
  align-items: center;
  gap: 0.08rem;
  transition: opacity 0.2s ease, transform 0.2s ease;
}

.axis-letter strong {
  font-size: 1.15rem;
  line-height: 1;
  letter-spacing: 0.12em;
  color: #c5d2de;
  text-shadow: 0 0 14px rgba(136, 157, 180, 0.4);
}

.axis-letter span {
  font-size: 0.64rem;
  letter-spacing: 0.08em;
  text-transform: uppercase;
  color: #8d9eb1;
}

.orbit-layer {
  position: absolute;
  inset: 0;
  z-index: 4;
  pointer-events: none;
  opacity: 0.95;
}

.orbit-node {
  position: absolute;
  transform: translate(-50%, -50%);
  pointer-events: auto;
  display: flex;
  align-items: center;
  gap: 0.42rem;
  color: #a7b6c6;
  max-width: min(36vw, 260px);
  transition: opacity 0.2s ease;
}

.node-dot {
  width: 8px;
  height: 8px;
  border-radius: 50%;
  background: #9cb2c8;
  box-shadow: 0 0 10px rgba(120, 141, 165, 0.7);
}

.node-label {
  font-size: 0.7rem;
  letter-spacing: 0.06em;
  text-transform: uppercase;
  white-space: nowrap;
  overflow: hidden;
  text-overflow: ellipsis;
  max-width: min(24vw, 180px);
}

.node-card {
  position: absolute;
  top: 1.2rem;
  left: 50%;
  width: min(236px, 32vw);
  min-width: 200px;
  transform: translateX(-50%);
  border-radius: 12px;
  border: 1px solid rgba(122, 144, 170, 0.2);
  background: rgba(15, 22, 30, 0.9);
  backdrop-filter: blur(12px);
  padding: 0.78rem 0.88rem;
  pointer-events: none;
  transition: opacity 0.2s ease, transform 0.2s ease;
  overflow-wrap: anywhere;
  word-break: break-word;
}

.node-card-icon {
  display: inline-flex;
  align-items: center;
  justify-content: center;
  min-width: 2.15rem;
  height: 1.5rem;
  border-radius: 999px;
  padding: 0 0.45rem;
  margin-bottom: 0.45rem;
  font-size: 0.63rem;
  letter-spacing: 0.06em;
  color: #c5d2de;
  border: 1px solid rgba(122, 144, 170, 0.45);
  background: rgba(122, 144, 170, 0.13);
}

.node-card h3 {
  margin: 0 0 0.2rem;
  font-size: 0.87rem;
  font-weight: 600;
  color: #e7edf5;
}

.node-card p {
  margin: 0;
  font-size: 0.75rem;
  line-height: 1.5;
  color: #9aaabc;
}

.code-orb {
  position: absolute;
  right: clamp(1rem, 4vw, 3rem);
  bottom: 4rem;
  z-index: 5;
  pointer-events: auto;
}

.code-orb-trigger {
  display: inline-flex;
  align-items: center;
  gap: 0.4rem;
  height: 1.72rem;
  padding: 0 0.58rem;
  border-radius: 999px;
  border: 1px solid rgba(122, 144, 170, 0.32);
  background: rgba(21, 30, 40, 0.64);
  color: #b8c7d6;
  font-size: 0.65rem;
  letter-spacing: 0.05em;
  box-shadow: 0 8px 18px rgba(4, 8, 12, 0.45);
  transition: transform 0.22s ease, background 0.22s ease, border-color 0.22s ease;
}

.code-orb-dot {
  width: 0.34rem;
  height: 0.34rem;
  border-radius: 50%;
  background: #adc2d7;
  box-shadow: 0 0 10px rgba(157, 178, 200, 0.75);
}

.code-orb-text {
  white-space: nowrap;
}

.code-orb-panel {
  position: absolute;
  right: 0;
  bottom: calc(100% + 0.58rem);
  width: min(500px, calc(100vw - 2.2rem));
  border-radius: 13px;
  border: 1px solid rgba(122, 144, 170, 0.24);
  background: rgba(15, 22, 30, 0.92);
  backdrop-filter: blur(16px);
  overflow: hidden;
  opacity: 0;
  transform: translateY(8px) scale(0.97);
  pointer-events: none;
  transition: opacity 0.22s ease, transform 0.22s ease;
}

.code-orb:hover .code-orb-trigger,
.code-orb:focus-within .code-orb-trigger {
  transform: translateY(-2px);
  background: rgba(37, 53, 70, 0.9);
  border-color: rgba(142, 166, 190, 0.62);
}

.code-orb:hover .code-orb-panel,
.code-orb:focus-within .code-orb-panel {
  opacity: 1;
  transform: translateY(0) scale(1);
  pointer-events: auto;
}

.code-orb-head {
  height: 34px;
  display: flex;
  align-items: center;
  padding: 0 0.9rem;
  font-size: 0.74rem;
  color: #b8c7d6;
  border-bottom: 1px solid rgba(122, 144, 170, 0.24);
  background: rgba(21, 30, 40, 0.8);
}

.code-orb-panel pre {
  margin: 0;
  padding: 0.85rem 0.95rem;
  overflow-x: auto;
}

.code-orb-panel code {
  font-family: 'JetBrains Mono', 'Fira Code', monospace;
  font-size: 0.74rem;
  line-height: 1.5;
  color: #d8e1eb;
}

.home-footer {
  position: absolute;
  left: 50%;
  bottom: 0.8rem;
  transform: translateX(-50%);
  text-align: center;
  z-index: 5;
  width: max-content;
  max-width: calc(100vw - 2rem);
}

.home-footer p {
  margin: 0;
  font-size: 0.72rem;
  line-height: 1.5;
  color: #8594a5;
  overflow-wrap: anywhere;
}

@media (max-width: 768px) {
  .home-root {
    min-height: 100vh;
    height: auto;
    overflow-x: hidden;
    overflow-y: auto;
    padding-bottom: 1.6rem;
  }

  .hero-shell {
    min-height: 100vh;
    height: auto;
    padding: 4.4rem 1rem 1rem;
  }

  .hero-center {
    position: relative;
    top: auto;
    left: auto;
    transform: none;
    width: 100%;
    padding: 0;
    border: none;
    background: transparent;
    backdrop-filter: none;
    box-shadow: none;
    animation: none;
  }

  .hero-title {
    font-size: 1.2rem;
    line-height: 1.5;
  }

  .hero-subtitle {
    font-size: 0.9rem;
    margin-bottom: 1.2rem;
  }

  .hero-actions {
    width: 100%;
    flex-direction: column;
  }

  .btn-primary,
  .btn-secondary {
    width: 100%;
  }

  .mobile-features {
    margin-top: 1.5rem;
    display: grid;
    grid-template-columns: repeat(2, minmax(0, 1fr));
    gap: 0.7rem;
  }

  .mobile-feature-card {
    border: 1px solid rgba(122, 144, 170, 0.2);
    background: rgba(17, 24, 33, 0.8);
    backdrop-filter: blur(10px);
    border-radius: 10px;
    padding: 0.7rem;
    overflow-wrap: anywhere;
    word-break: break-word;
  }

  .mobile-icon {
    display: inline-flex;
    align-items: center;
    justify-content: center;
    height: 1.35rem;
    min-width: 2rem;
    border-radius: 999px;
    border: 1px solid rgba(122, 144, 170, 0.4);
    color: #c3d1de;
    font-size: 0.62rem;
    margin-bottom: 0.45rem;
  }

  .mobile-feature-card h3 {
    margin: 0 0 0.2rem;
    font-size: 0.84rem;
    color: #e7edf5;
  }

  .mobile-feature-card p {
    margin: 0;
    font-size: 0.73rem;
    line-height: 1.45;
    color: #95a6b8;
  }

  .code-orb {
    display: none;
  }

  .home-footer {
    position: static;
    margin-top: 1rem;
    text-align: center;
    max-width: none;
    transform: none;
    width: auto;
  }
}

@media (max-width: 460px) {
  .mobile-features {
    grid-template-columns: 1fr;
  }

  .hero-badge {
    letter-spacing: 0.11em;
    font-size: 0.68rem;
  }

}
</style>
