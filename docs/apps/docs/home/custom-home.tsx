/**
 * User-owned homepage entry.
 * This file is protected by create-nexdoc upgrade.
 */
"use client";

import Link from "next/link";
import { useCallback, useEffect, useMemo, useRef, useState } from "react";
import type { ProjectGroup } from "@/lib/docs";

type AxisDirection = "x+" | "x-" | "y+" | "y-" | "z+" | "z-";

interface AmbientParticle {
  x: number;
  y: number;
  z: number;
  vx: number;
  vy: number;
  vz: number;
  baseRadius: number;
  baseBrightness: number;
  axisIndex: 0 | 1 | 2;
  axisPos: number;
  jitterA: number;
  jitterB: number;
  color: string;
}

interface GlyphParticle {
  lx: number;
  ly: number;
  lz: number;
  ax: number;
  ay: number;
  az: number;
  x: number;
  y: number;
  z: number;
  baseRadius: number;
  baseBrightness: number;
  color: string;
}

interface FeatureData {
  icon: string;
  titleEn: string;
  titleZh: string;
  descEn: string;
  descZh: string;
  axis: AxisDirection;
}

const features: FeatureData[] = [
  {
    icon: "IO",
    titleEn: "Throughput Core",
    titleZh: "高吞吐核心",
    descEn:
      "Segmented storage and parallel IO deliver sustained throughput with predictable latency under heavy concurrency.",
    descZh:
      "段式存储与并行 IO 协同优化，在高并发负载下提供持续吞吐与可预测时延。",
    axis: "x+",
  },
  {
    icon: "TX",
    titleEn: "ACID Transactions",
    titleZh: "ACID 事务",
    descEn:
      "WAL, optimistic concurrency control, and rollbackable state chains preserve correctness for multi-step graph mutations.",
    descZh:
      "WAL、乐观并发控制与可回滚状态链协同，保障多步骤图更新的 ACID 正确性。",
    axis: "x-",
  },
  {
    icon: "CY",
    titleEn: "Cypher Native",
    titleZh: "Cypher 原生",
    descEn:
      "Parser, planner, and optimizer are purpose-built for graph semantics and complex pattern matching.",
    descZh:
      "解析器、规划器与优化器围绕图语义原生设计，复杂模式匹配依然高效。",
    axis: "y+",
  },
  {
    icon: "VS",
    titleEn: "Graph + Vector",
    titleZh: "图向量融合",
    descEn:
      "DiskANN and quantization fuse ANN retrieval with graph traversal in a unified query path for AI workloads.",
    descZh:
      "DiskANN 与量化索引将 ANN 检索与图遍历融合到统一查询链路，面向 AI 负载原生优化。",
    axis: "y-",
  },
  {
    icon: "EM",
    titleEn: "Embedded Runtime",
    titleZh: "嵌入式运行时",
    descEn:
      "Integrate directly through C/C++ APIs with no standalone server or daemon process.",
    descZh: "通过 C/C++ API 直接集成，无需部署独立服务器或守护进程。",
    axis: "z+",
  },
  {
    icon: "OS",
    titleEn: "Open and Governable",
    titleZh: "开源可控",
    descEn:
      "MIT-licensed architecture with transparent evolution for enterprise governance, auditing, and customization.",
    descZh:
      "MIT 开源许可与透明演进路径，支持企业级治理、审计、定制与长期迭代。",
    axis: "z-",
  },
];

const BG = "#0b0f14";
const AXIS_A = "#3a4a5e";
const AXIS_B = "#6b829c";
const FAR_RGB: [number, number, number] = [30, 40, 52];
const NEAR_RGB: [number, number, number] = [148, 168, 190];
const AXIS_LENGTH = 550;
const MORPH_CYCLE = 2400;
const BASE_YAW = -0.2;

const clamp = (value: number, min: number, max: number) =>
  Math.max(min, Math.min(max, value));

const smoothstep = (edge0: number, edge1: number, value: number) => {
  const t = clamp((value - edge0) / (edge1 - edge0), 0, 1);
  return t * t * (3 - 2 * t);
};

function shuffle<T>(array: T[]) {
  for (let i = array.length - 1; i > 0; i -= 1) {
    const j = Math.floor(Math.random() * (i + 1));
    [array[i], array[j]] = [array[j], array[i]];
  }
}

function projectAxis(
  x: number,
  y: number,
  z: number,
  cosX: number,
  sinX: number,
  cosY: number,
  sinY: number,
  cx: number,
  cy: number,
  fov: number,
) {
  const x1 = x * cosY - z * sinY;
  const z1 = x * sinY + z * cosY;
  const y2 = y * cosX - z1 * sinX;
  const z2 = y * sinX + z1 * cosX;
  const scale = fov / (fov + z2);
  return { sx: cx + x1 * scale, sy: cy + y2 * scale };
}

function drawAxes(
  ctx: CanvasRenderingContext2D,
  cosX: number,
  sinX: number,
  cosY: number,
  sinY: number,
  cx: number,
  cy: number,
  fov: number,
  alpha: number,
) {
  if (alpha <= 0.01) {
    return;
  }

  ctx.globalAlpha = alpha * 0.4;
  const axes = [
    [1, 0, 0],
    [0, 1, 0],
    [0, 0, 1],
  ];

  axes.forEach((axis) => {
    const start = projectAxis(
      -axis[0] * AXIS_LENGTH,
      -axis[1] * AXIS_LENGTH,
      -axis[2] * AXIS_LENGTH,
      cosX,
      sinX,
      cosY,
      sinY,
      cx,
      cy,
      fov,
    );

    const end = projectAxis(
      axis[0] * AXIS_LENGTH,
      axis[1] * AXIS_LENGTH,
      axis[2] * AXIS_LENGTH,
      cosX,
      sinX,
      cosY,
      sinY,
      cx,
      cy,
      fov,
    );

    const gradient = ctx.createLinearGradient(start.sx, start.sy, end.sx, end.sy);
    gradient.addColorStop(0, AXIS_A);
    gradient.addColorStop(0.5, AXIS_B);
    gradient.addColorStop(1, AXIS_A);

    ctx.strokeStyle = gradient;
    ctx.lineWidth = 1;
    ctx.beginPath();
    ctx.moveTo(start.sx, start.sy);
    ctx.lineTo(end.sx, end.sy);
    ctx.stroke();
  });
}

function HudFeatureCard({
  feature,
  isEn,
  animation,
}: {
  feature: FeatureData;
  isEn: boolean;
  animation: string;
}) {
  return (
    <div
      className="absolute inset-0 flex h-full w-full flex-col items-center text-center"
      style={{ animation }}
    >
      <div className="mb-[0.6rem] font-['Space_Mono','Ubuntu_Mono',monospace] text-[0.75rem] font-semibold tracking-[0.15em] text-[#94a8be] [text-shadow:0_0_10px_rgba(148,168,190,0.4)]">
        [ {feature.icon} ]
      </div>
      <h3 className="mb-[0.4rem] mt-0 text-[1.1rem] font-semibold tracking-[0.03em] text-[#e7edf5]">
        {isEn ? feature.titleEn : feature.titleZh}
      </h3>
      <p className="m-0 max-w-[480px] overflow-hidden text-[0.85rem] leading-[1.5] text-[#8a9bb0] [display:-webkit-box] [-webkit-box-orient:vertical] [-webkit-line-clamp:2] [text-overflow:ellipsis]">
        {isEn ? feature.descEn : feature.descZh}
      </p>
    </div>
  );
}

export interface ZyxHomePageProps {
  locale: string;
  firstDocHref: string;
  projects: ProjectGroup[];
  jsonLd: Record<string, unknown>;
}

export function ZyxHomePage(props: ZyxHomePageProps) {
  const { locale, jsonLd } = props;
  const isEn = locale === "en-US" || locale === "en";

  const quickStartLink = useMemo(
    () => (isEn ? "/en/docs/zyx/user-guide/quick-start" : "/zh/docs/zyx/user-guide/quick-start"),
    [isEn],
  );

  const canvasRef = useRef<HTMLCanvasElement | null>(null);
  const cycleTimerRef = useRef<ReturnType<typeof setInterval> | null>(null);
  const leaveResetTimerRef = useRef<ReturnType<typeof setTimeout> | null>(null);

  const animationIdRef = useRef<number | null>(null);
  const tickRef = useRef<(() => void) | null>(null);
  const isAnimatingRef = useRef(false);
  const ctxRef = useRef<CanvasRenderingContext2D | null>(null);

  const widthRef = useRef(0);
  const heightRef = useRef(0);
  const dprRef = useRef(1);
  const frameCountRef = useRef(0);

  const currentMouseYawRef = useRef(0);
  const currentMousePitchRef = useRef(0);
  const targetMouseYawRef = useRef(0);
  const targetMousePitchRef = useRef(0);

  const ambientParticlesRef = useRef<AmbientParticle[]>([]);
  const glyphParticlesRef = useRef<GlyphParticle[]>([]);

  const isMobileRef = useRef(false);
  const [isMobile, setIsMobile] = useState(false);

  const [activeFeatureIndex, setActiveFeatureIndex] = useState(0);
  const [leavingFeatureIndex, setLeavingFeatureIndex] = useState<number | null>(
    null,
  );

  const updateMobileFlag = useCallback(() => {
    const nextIsMobile = window.innerWidth <= 768;
    isMobileRef.current = nextIsMobile;
    setIsMobile(nextIsMobile);
    return nextIsMobile;
  }, []);

  const createGlyphTargets = useCallback((count: number) => {
    const canvas = document.createElement("canvas");
    const cw = 2000;
    const ch = 1000;
    canvas.width = cw;
    canvas.height = ch;

    const textCtx = canvas.getContext("2d", { willReadFrequently: true });
    if (!textCtx) {
      return [] as Array<{ x: number; y: number; z: number }>;
    }

    textCtx.fillStyle = "#fff";
    textCtx.textAlign = "center";
    textCtx.textBaseline = "middle";
    textCtx.font = 'bold 700px "Space Mono", "Ubuntu Mono", monospace';
    textCtx.fillText("ZYX", cw * 0.5, ch * 0.5);

    const data = textCtx.getImageData(0, 0, cw, ch).data;
    const points: Array<{ x: number; y: number }> = [];

    let minX = cw;
    let maxX = 0;
    let minY = ch;
    let maxY = 0;

    for (let y = 0; y < ch; y += 7) {
      for (let x = 0; x < cw; x += 7) {
        if (data[(y * cw + x) * 4 + 3] > 120) {
          points.push({ x, y });
          if (x < minX) minX = x;
          if (x > maxX) maxX = x;
          if (y < minY) minY = y;
          if (y > maxY) maxY = y;
        }
      }
    }

    if (points.length === 0) {
      return [] as Array<{ x: number; y: number; z: number }>;
    }

    shuffle(points);

    const exactCenterX = (minX + maxX) / 2;
    const exactCenterY = (minY + maxY) / 2;
    const scale = isMobileRef.current ? 0.6 : 1.15;

    const targets: Array<{ x: number; y: number; z: number }> = [];
    for (let i = 0; i < count; i += 1) {
      const point = points[i % points.length];
      targets.push({
        x: (point.x - exactCenterX) * scale,
        y: (point.y - exactCenterY) * scale,
        z: (Math.random() - 0.5) * 120,
      });
    }

    return targets;
  }, []);

  const resetParticles = useCallback(() => {
    const ambientParticles = ambientParticlesRef.current;
    const glyphParticles = glyphParticlesRef.current;
    ambientParticles.length = 0;
    glyphParticles.length = 0;

    const ambientCount = isMobileRef.current ? 500 : 1500;
    const glyphCount = isMobileRef.current ? 400 : 1000;

    for (let i = 0; i < ambientCount; i += 1) {
      const axisIndex = Math.floor(Math.random() * 3) as 0 | 1 | 2;
      const axisPos = (Math.random() * 2 - 1) * AXIS_LENGTH * 1.5;
      const spread = Math.pow(Math.random(), 0.8) * 200 + 10;
      const angle = Math.random() * Math.PI * 2;
      const brightness = clamp(1 - spread / 250, 0.1, 1);

      const r = ~~(FAR_RGB[0] + (NEAR_RGB[0] - FAR_RGB[0]) * brightness);
      const g = ~~(FAR_RGB[1] + (NEAR_RGB[1] - FAR_RGB[1]) * brightness);
      const b = ~~(FAR_RGB[2] + (NEAR_RGB[2] - FAR_RGB[2]) * brightness);

      ambientParticles.push({
        x: 0,
        y: 0,
        z: 0,
        vx: 0,
        vy: 0,
        vz: 0,
        baseRadius: Math.random() * 1.0 + 0.5,
        baseBrightness: brightness,
        axisIndex,
        axisPos,
        jitterA: Math.cos(angle) * spread,
        jitterB: Math.sin(angle) * spread,
        color: `rgb(${r},${g},${b})`,
      });
    }

    const targets = createGlyphTargets(glyphCount);
    for (let i = 0; i < glyphCount; i += 1) {
      const glyph = targets[i] || { x: 0, y: 0, z: 0 };
      const brightness = 0.7 + Math.random() * 0.3;
      const r = ~~(FAR_RGB[0] + (NEAR_RGB[0] - FAR_RGB[0]) * brightness);
      const g = ~~(FAR_RGB[1] + (NEAR_RGB[1] - FAR_RGB[1]) * brightness);
      const b = ~~(FAR_RGB[2] + (NEAR_RGB[2] - FAR_RGB[2]) * brightness);

      glyphParticles.push({
        lx: glyph.x,
        ly: glyph.y,
        lz: glyph.z,
        ax: (Math.random() - 0.5) * AXIS_LENGTH * 2,
        ay: (Math.random() - 0.5) * AXIS_LENGTH * 1.2,
        az: (Math.random() - 0.5) * AXIS_LENGTH * 2,
        x: 0,
        y: 0,
        z: 0,
        baseRadius: isMobileRef.current ? 1.0 : 1.2,
        baseBrightness: brightness,
        color: `rgb(${r},${g},${b})`,
      });
    }
  }, [createGlyphTargets]);

  const resizeCanvas = useCallback(() => {
    const canvas = canvasRef.current;
    const ctx = ctxRef.current;
    if (!canvas || !ctx) {
      return;
    }

    widthRef.current = window.innerWidth;
    heightRef.current = window.innerHeight;
    dprRef.current = Math.min(window.devicePixelRatio || 1, 2);

    canvas.width = Math.floor(widthRef.current * dprRef.current);
    canvas.height = Math.floor(heightRef.current * dprRef.current);
    canvas.style.width = `${widthRef.current}px`;
    canvas.style.height = `${heightRef.current}px`;

    ctx.setTransform(dprRef.current, 0, 0, dprRef.current, 0, 0);
    updateMobileFlag();
    resetParticles();
  }, [resetParticles, updateMobileFlag]);

  const transitionToNextFeature = useCallback(() => {
    setActiveFeatureIndex((current) => {
      const next = (current + 1) % features.length;
      setLeavingFeatureIndex(current);

      if (leaveResetTimerRef.current) {
        clearTimeout(leaveResetTimerRef.current);
      }

      leaveResetTimerRef.current = setTimeout(() => {
        setLeavingFeatureIndex(null);
      }, 620);

      return next;
    });
  }, []);

  const startAutoCycle = useCallback(() => {
    if (isMobileRef.current) {
      return;
    }

    if (cycleTimerRef.current) {
      clearInterval(cycleTimerRef.current);
    }

    cycleTimerRef.current = setInterval(() => {
      transitionToNextFeature();
    }, 4500);
  }, [transitionToNextFeature]);

  const stopAutoCycle = useCallback(() => {
    if (!cycleTimerRef.current) {
      return;
    }

    clearInterval(cycleTimerRef.current);
    cycleTimerRef.current = null;
  }, []);

  const stopAnimation = useCallback(() => {
    isAnimatingRef.current = false;
    if (animationIdRef.current === null) {
      return;
    }

    window.cancelAnimationFrame(animationIdRef.current);
    animationIdRef.current = null;
  }, []);

  const startAnimation = useCallback(() => {
    if (isAnimatingRef.current) {
      return;
    }

    const tick = tickRef.current;
    if (!tick) {
      return;
    }

    isAnimatingRef.current = true;
    animationIdRef.current = window.requestAnimationFrame(tick);
  }, []);

  useEffect(() => {
    if (isMobile) {
      stopAutoCycle();
      return;
    }

    startAutoCycle();
    return () => {
      stopAutoCycle();
    };
  }, [isMobile, startAutoCycle, stopAutoCycle]);

  useEffect(() => {
    const ctx = canvasRef.current?.getContext("2d", { alpha: false });
    if (!ctx) {
      return;
    }

    ctxRef.current = ctx;
    resizeCanvas();

    const handleMouseMove = (event: MouseEvent) => {
      if (widthRef.current === 0 || heightRef.current === 0) {
        return;
      }

      targetMouseYawRef.current = (event.clientX / widthRef.current - 0.5) * 0.3;
      targetMousePitchRef.current =
        -(event.clientY / heightRef.current - 0.5) * 0.2;
    };

    const handleMouseLeave = () => {
      targetMouseYawRef.current = 0;
      targetMousePitchRef.current = 0;
    };

    const tick = () => {
      const canvasCtx = ctxRef.current;
      if (!canvasCtx) {
        return;
      }

      frameCountRef.current += 1;

      const width = widthRef.current;
      const height = heightRef.current;

      canvasCtx.globalAlpha = 1;
      canvasCtx.fillStyle = BG;
      canvasCtx.fillRect(0, 0, width, height);

      currentMouseYawRef.current +=
        (targetMouseYawRef.current - currentMouseYawRef.current) * 0.05;
      currentMousePitchRef.current +=
        (targetMousePitchRef.current - currentMousePitchRef.current) * 0.05;

      const rx = currentMousePitchRef.current;
      const ry = BASE_YAW + currentMouseYawRef.current;
      const cosX = Math.cos(rx);
      const sinX = Math.sin(rx);
      const cosY = Math.cos(ry);
      const sinY = Math.sin(ry);
      const cx = width * 0.5;
      const cy = height * 0.5;
      const fov = 1000;

      const t = (frameCountRef.current % MORPH_CYCLE) / MORPH_CYCLE;
      const gather = smoothstep(0.4, 0.55, t);
      const release = smoothstep(0.75, 0.9, t);
      const glyphMorph = clamp(gather - release, 0, 1);
      const axisAlpha = smoothstep(0.1, 0.5, 1 - glyphMorph);

      const ambientParticles = ambientParticlesRef.current;
      for (let i = 0; i < ambientParticles.length; i += 1) {
        const particle = ambientParticles[i];
        const wobble =
          1 + Math.sin(frameCountRef.current * 0.005 + particle.axisPos * 0.01) * 0.05;

        let tx = 0;
        let ty = 0;
        let tz = 0;

        if (particle.axisIndex === 0) {
          tx = particle.axisPos;
          ty = particle.jitterA * wobble;
          tz = particle.jitterB * wobble;
        } else if (particle.axisIndex === 1) {
          tx = particle.jitterA * wobble;
          ty = particle.axisPos;
          tz = particle.jitterB * wobble;
        } else {
          tx = particle.jitterA * wobble;
          ty = particle.jitterB * wobble;
          tz = particle.axisPos;
        }

        particle.x += (tx - particle.x) * 0.06;
        particle.y += (ty - particle.y) * 0.06;
        particle.z += (tz - particle.z) * 0.06;

        const x1 = particle.x * cosY - particle.z * sinY;
        const z1 = particle.x * sinY + particle.z * cosY;
        const y2 = particle.y * cosX - z1 * sinX;
        const z2 = particle.y * sinX + z1 * cosX;

        if (z2 <= -fov + 20) {
          continue;
        }

        const scale = fov / (fov + z2);
        canvasCtx.globalAlpha = (0.2 + particle.baseBrightness * 0.4) * axisAlpha;
        canvasCtx.fillStyle = particle.color;

        const size = particle.baseRadius * scale * 2;
        canvasCtx.fillRect(
          cx + x1 * scale - size * 0.5,
          cy + y2 * scale - size * 0.5,
          size,
          size,
        );
      }

      const glyphParticles = glyphParticlesRef.current;
      for (let i = 0; i < glyphParticles.length; i += 1) {
        const particle = glyphParticles[i];
        const tx = particle.ax + (particle.lx - particle.ax) * glyphMorph;
        const ty = particle.ay + (particle.ly - particle.ay) * glyphMorph;
        const tz = particle.az + (particle.lz - particle.az) * glyphMorph;

        particle.x += (tx - particle.x) * 0.08;
        particle.y += (ty - particle.y) * 0.08;
        particle.z += (tz - particle.z) * 0.08;

        const x1 = particle.x * cosY - particle.z * sinY;
        const z1 = particle.x * sinY + particle.z * cosY;
        const y2 = particle.y * cosX - z1 * sinX;
        const z2 = particle.y * sinX + z1 * cosX;

        if (z2 <= -fov + 20) {
          continue;
        }

        const scale = fov / (fov + z2);
        canvasCtx.globalAlpha = 0.3 + glyphMorph * 0.6;
        canvasCtx.fillStyle = particle.color;

        const size = particle.baseRadius * scale * (1 + glyphMorph * 0.3) * 2;
        canvasCtx.fillRect(
          cx + x1 * scale - size * 0.5,
          cy + y2 * scale - size * 0.5,
          size,
          size,
        );
      }

      drawAxes(canvasCtx, cosX, sinX, cosY, sinY, cx, cy, fov, axisAlpha);
      if (!isAnimatingRef.current) {
        return;
      }

      animationIdRef.current = window.requestAnimationFrame(tick);
    };

    window.addEventListener("resize", resizeCanvas);
    window.addEventListener("mousemove", handleMouseMove);
    window.addEventListener("mouseleave", handleMouseLeave);
    tickRef.current = tick;
    startAnimation();

    return () => {
      stopAnimation();
      tickRef.current = null;
      stopAutoCycle();
      window.removeEventListener("resize", resizeCanvas);
      window.removeEventListener("mousemove", handleMouseMove);
      window.removeEventListener("mouseleave", handleMouseLeave);

      if (leaveResetTimerRef.current) {
        clearTimeout(leaveResetTimerRef.current);
        leaveResetTimerRef.current = null;
      }
    };
  }, [resizeCanvas, startAnimation, stopAnimation, stopAutoCycle]);

  useEffect(() => {
    const restartIfVisible = () => {
      if (document.visibilityState !== "visible") {
        return;
      }

      startAnimation();
      if (!isMobileRef.current) {
        startAutoCycle();
      }
    };

    const handlePageShow = () => {
      restartIfVisible();
    };

    const handlePageHide = () => {
      stopAnimation();
      stopAutoCycle();
    };

    const handleVisibilityChange = () => {
      if (document.visibilityState === "hidden") {
        stopAnimation();
        stopAutoCycle();
        return;
      }

      restartIfVisible();
    };

    window.addEventListener("pageshow", handlePageShow);
    window.addEventListener("pagehide", handlePageHide);
    document.addEventListener("visibilitychange", handleVisibilityChange);

    return () => {
      window.removeEventListener("pageshow", handlePageShow);
      window.removeEventListener("pagehide", handlePageHide);
      document.removeEventListener("visibilitychange", handleVisibilityChange);
    };
  }, [startAnimation, stopAnimation, startAutoCycle, stopAutoCycle]);

  const activeFeature = features[activeFeatureIndex];
  const leavingFeature =
    leavingFeatureIndex === null ? null : features[leavingFeatureIndex];

  return (
    <>
      <style jsx global>{`
        @keyframes data-feed-enter {
          0% {
            opacity: 0;
            transform: translateY(15px) scale(0.98);
            filter: blur(8px);
          }
          100% {
            opacity: 1;
            transform: translateY(0) scale(1);
            filter: blur(0);
          }
        }

        @keyframes data-feed-leave {
          0% {
            opacity: 1;
            transform: translateY(0) scale(1);
            filter: blur(0);
          }
          100% {
            opacity: 0;
            transform: translateY(-15px) scale(1.02);
            filter: blur(8px);
          }
        }
      `}</style>

      <div className="relative h-screen w-screen overflow-hidden bg-[#0b0f14] font-['Avenir_Next','Segoe_UI',system-ui,sans-serif] text-[#e7edf5] supports-[height:100dvh]:h-dvh">
        <canvas ref={canvasRef} className="pointer-events-auto absolute inset-0 z-0" />

        <div className="pointer-events-none absolute inset-0 z-[1] bg-[radial-gradient(circle_at_50%_50%,rgba(11,15,20,0.6)_0%,transparent_60%)]" />

        <div className="pointer-events-none absolute inset-0 z-[2] flex flex-col">
          <main className="pointer-events-auto absolute left-1/2 top-[40%] flex w-[calc(100vw-2.4rem)] -translate-x-1/2 -translate-y-1/2 flex-col items-center text-center md:top-[52%] md:w-[min(860px,92vw)]">
            <div className="mb-[1.2rem] inline-flex rounded-[50px] border border-[rgba(122,144,170,0.3)] bg-[rgba(22,30,39,0.4)] px-4 py-[0.48rem] text-[0.72rem] font-medium uppercase tracking-[0.16em] text-[#aec0d2] backdrop-blur-[4px]">
              {isEn ? "EMBEDDED GRAPH DATABASE" : "嵌入式图数据库"}
            </div>

            <h1 className="m-0 text-[1.6rem] font-semibold leading-[1.15] tracking-[0.02em] text-[#e7edf5] md:text-[clamp(2rem,4vw,3.8rem)]">
              {isEn ? "AI-Native Graph Engine" : "AI 原生图引擎"}
            </h1>

            <p className="mb-8 mt-[1.2rem] text-[0.95rem] leading-[1.6] text-[#9db0c4] md:text-[clamp(0.95rem,1.4vw,1.05rem)]">
              {isEn ? (
                <>
                  Unify graph reasoning, vector retrieval, and ACID transactions in
                  one local runtime.
                  <br />
                  Built for RAG, agent memory, and knowledge graph applications with
                  low-latency control, no standalone server required. All data lives
                  in a single database file.
                </>
              ) : (
                <>
                  在单一本地引擎中统一图推理、向量检索与 ACID 事务。
                  <br />
                  为 RAG、Agent 与知识图谱应用提供可控、低延迟的数据底座，无需独立服务器。单个数据库文件持久化存储全量数据。
                </>
              )}
            </p>

            <div className="mb-0 flex w-full flex-col gap-4 md:mb-14 md:w-auto md:flex-row">
              <Link
                href={quickStartLink}
                className="inline-flex w-full min-w-[140px] items-center justify-center gap-2 rounded-[8px] border border-[rgba(122,144,170,0.7)] bg-[rgba(35,48,63,0.6)] px-[1.4rem] py-3 text-[0.88rem] text-[#e7edf5] no-underline backdrop-blur-[4px] transition-all duration-[250ms] ease-in-out hover:bg-[#778ea7] hover:text-[#0b0f14] md:w-auto"
              >
                {isEn ? "Get Started" : "快速开始"}
                <span aria-hidden="true">&rarr;</span>
              </Link>

              <a
                href="https://github.com/nexepic/zyx"
                target="_blank"
                rel="noreferrer"
                className="inline-flex w-full min-w-[140px] items-center justify-center gap-2 rounded-[8px] border border-[rgba(122,144,170,0.3)] bg-[rgba(24,34,44,0.4)] px-[1.4rem] py-3 text-[0.88rem] text-[#b8c7d6] no-underline backdrop-blur-[4px] transition-all duration-[250ms] ease-in-out hover:bg-[rgba(118,142,167,0.2)] hover:text-white md:w-auto"
              >
                GitHub ↗
              </a>
            </div>

            {!isMobile && (
              <div
                className="group relative w-full max-w-[580px] bg-[radial-gradient(ellipse_at_center,rgba(148,168,190,0.06)_0%,transparent_70%)] py-6"
                onMouseEnter={stopAutoCycle}
                onMouseLeave={startAutoCycle}
              >
                <span className="absolute left-0 top-0 h-3 w-3 border-l border-t border-[rgba(122,144,170,0.4)] transition-[border-color,box-shadow] duration-300 ease-in-out group-hover:border-[rgba(148,168,190,0.8)] group-hover:[box-shadow:0_0_8px_rgba(148,168,190,0.2)]" />
                <span className="absolute right-0 top-0 h-3 w-3 border-r border-t border-[rgba(122,144,170,0.4)] transition-[border-color,box-shadow] duration-300 ease-in-out group-hover:border-[rgba(148,168,190,0.8)] group-hover:[box-shadow:0_0_8px_rgba(148,168,190,0.2)]" />
                <span className="absolute bottom-0 left-0 h-3 w-3 border-b border-l border-[rgba(122,144,170,0.4)] transition-[border-color,box-shadow] duration-300 ease-in-out group-hover:border-[rgba(148,168,190,0.8)] group-hover:[box-shadow:0_0_8px_rgba(148,168,190,0.2)]" />
                <span className="absolute bottom-0 right-0 h-3 w-3 border-b border-r border-[rgba(122,144,170,0.4)] transition-[border-color,box-shadow] duration-300 ease-in-out group-hover:border-[rgba(148,168,190,0.8)] group-hover:[box-shadow:0_0_8px_rgba(148,168,190,0.2)]" />

                <div className="relative h-[115px] w-full overflow-hidden">
                  {leavingFeature && leavingFeatureIndex !== activeFeatureIndex && (
                    <HudFeatureCard
                      feature={leavingFeature}
                      isEn={isEn}
                      animation="data-feed-leave 0.6s cubic-bezier(0.16, 1, 0.3, 1) forwards"
                    />
                  )}

                  <HudFeatureCard
                    key={`${activeFeature.titleEn}-${activeFeatureIndex}`}
                    feature={activeFeature}
                    isEn={isEn}
                    animation="data-feed-enter 0.6s cubic-bezier(0.16, 1, 0.3, 1) forwards"
                  />
                </div>
              </div>
            )}
          </main>

          {isMobile && (
            <div className="pointer-events-auto absolute bottom-8 left-0 right-0 flex gap-[0.8rem] overflow-x-auto px-[1.2rem] [scrollbar-width:none] [-ms-overflow-style:none] [&::-webkit-scrollbar]:hidden">
              {features.map((feature) => (
                <div
                  key={feature.titleEn}
                  className="max-w-[280px] flex-[0_0_80%] rounded-[12px] border border-[rgba(122,144,170,0.2)] bg-[rgba(16,22,30,0.75)] p-[1.1rem] backdrop-blur-[10px]"
                >
                  <div className="mb-[0.6rem] inline-block rounded-[50px] bg-[rgba(122,144,170,0.15)] px-[0.6rem] py-[0.2rem] text-[0.65rem] text-[#c5d2de]">
                    {feature.icon}
                  </div>
                  <h3 className="mb-[0.3rem] mt-0 text-[0.9rem] text-[#e7edf5]">
                    {isEn ? feature.titleEn : feature.titleZh}
                  </h3>
                  <p className="m-0 text-[0.8rem] leading-[1.45] text-[#8a9bb0]">
                    {isEn ? feature.descEn : feature.descZh}
                  </p>
                </div>
              ))}
            </div>
          )}

          <footer className="absolute bottom-[0.8rem] left-1/2 -translate-x-1/2 text-[0.7rem] text-[#566b82]">
            MIT License &copy; 2025 ZYX Contributors
          </footer>
        </div>

        <script
          type="application/ld+json"
          dangerouslySetInnerHTML={{ __html: JSON.stringify(jsonLd) }}
        />
      </div>
    </>
  );
}

export function CustomHomePage(props: ZyxHomePageProps) {
  return <ZyxHomePage {...props} />;
}

export default CustomHomePage
