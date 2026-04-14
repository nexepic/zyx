"use client";

import { useCallback, useEffect, useRef } from "react";
import {
  forceSimulation,
  forceLink,
  forceManyBody,
  forceCenter,
  forceCollide,
  type SimulationNodeDatum,
  type SimulationLinkDatum,
} from "d3-force";

export interface GraphNode {
  id: number;
  label: string;
}

export interface GraphEdge {
  id: number;
  sourceId: number;
  targetId: number;
  type: string;
}

interface SimNode extends SimulationNodeDatum {
  id: number;
  label: string;
  degree: number;
  radius: number;
  colorIndex: number;
}

interface SimLink extends SimulationLinkDatum<SimNode> {
  type: string;
}

interface Props {
  nodes: GraphNode[];
  edges: GraphEdge[];
}

// Professional, muted palette derived from the main site's slate/blue base
// Provides differentiation without looking like a neon lightshow
const NODE_PALETTE = [
  "#6b829c", // Base Slate Blue
  "#5c7e85", // Muted Teal
  "#7b6b85", // Muted Mauve
  "#85786b", // Muted Bronze
  "#5a6678", // Deep Slate
  "#748773", // Muted Sage
];

const LABEL_COLOR = "#e7edf5";
const EDGE_COLOR = "rgba(122,144,170,0.2)";
const EDGE_HIGHLIGHT_COLOR = "rgba(148,168,190,0.85)";

function clamp(v: number, lo: number, hi: number) {
  return Math.max(lo, Math.min(hi, v));
}

// Simple community detection via greedy graph coloring
function assignColors(nodes: SimNode[], links: SimLink[]): void {
  const adj = new Map<number, Set<number>>();
  for (const n of nodes) adj.set(n.id, new Set());
  for (const l of links) {
    const src = l.source as SimNode;
    const tgt = l.target as SimNode;
    adj.get(src.id)?.add(tgt.id);
    adj.get(tgt.id)?.add(src.id);
  }

  // Sort by degree descending for better coloring
  const sorted = [...nodes].sort((a, b) => b.degree - a.degree);
  const colorMap = new Map<number, number>();
  const paletteSize = NODE_PALETTE.length;

  for (const node of sorted) {
    const neighborColors = new Set<number>();
    for (const neighborId of adj.get(node.id) || []) {
      const c = colorMap.get(neighborId);
      if (c !== undefined) neighborColors.add(c);
    }
    // Pick first color not used by any neighbor
    let color = 0;
    while (neighborColors.has(color) && color < paletteSize) color++;
    if (color >= paletteSize) color = node.degree % paletteSize;
    colorMap.set(node.id, color);
    node.colorIndex = color;
  }
}

export function GraphView({ nodes, edges }: Props) {
  const canvasRef = useRef<HTMLCanvasElement>(null);
  const containerRef = useRef<HTMLDivElement>(null);

  const stateRef = useRef({
    simNodes: [] as SimNode[],
    simLinks: [] as SimLink[],
    transform: { x: 0, y: 0, k: 1 },
    drag: null as SimNode | null,
    dragOffset: { x: 0, y: 0 },
    pan: null as { startX: number; startY: number; originX: number; originY: number } | null,
    hover: null as SimNode | null,
    sim: null as ReturnType<typeof forceSimulation<SimNode>> | null,
    frame: 0,
    canvasW: 0,
    canvasH: 0,
  });

  // Build simulation
  useEffect(() => {
    if (!nodes.length) return;

    const s = stateRef.current;
    s.sim?.stop();

    const degreeMap = new Map<number, number>();
    for (const e of edges) {
      degreeMap.set(e.sourceId, (degreeMap.get(e.sourceId) || 0) + 1);
      degreeMap.set(e.targetId, (degreeMap.get(e.targetId) || 0) + 1);
    }
    const maxDegree = Math.max(1, ...degreeMap.values());

    const sNodes: SimNode[] = nodes.map((n) => {
      const deg = degreeMap.get(n.id) || 0;
      return {
        id: n.id,
        label: n.label,
        degree: deg,
        // Sensible sizing
        radius: 4 + 8 * (deg / maxDegree),
        colorIndex: 0,
      };
    });

    const nodeIndex = new Map(sNodes.map((n) => [n.id, n]));
    const sLinks: SimLink[] = [];
    for (const e of edges) {
      const src = nodeIndex.get(e.sourceId);
      const tgt = nodeIndex.get(e.targetId);
      if (src && tgt) {
        sLinks.push({ source: src, target: tgt, type: e.type });
      }
    }

    assignColors(sNodes, sLinks);

    s.simNodes = sNodes;
    s.simLinks = sLinks;
    const initialScale = nodes.length > 50 ? 0.7 : 1.0;
    s.transform = { x: 0, y: 0, k: initialScale };
    s.hover = null;
    s.drag = null;
    s.pan = null;

    // Relaxed layout physics to avoid clustering
    const sim = forceSimulation(sNodes)
      .force("link", forceLink<SimNode, SimLink>(sLinks).id((d) => d.id).distance(70).strength(0.5))
      .force("charge", forceManyBody().strength(-200))
      .force("center", forceCenter(0, 0))
      .force("collide", forceCollide<SimNode>().radius((d) => d.radius + 6));

    s.sim = sim;

    return () => {
      sim.stop();
      s.sim = null;
    };
  }, [nodes, edges]);

  const renderFrame = useCallback(() => {
    const canvas = canvasRef.current;
    if (!canvas) return;
    const ctx = canvas.getContext("2d");
    if (!ctx) return;

    const s = stateRef.current;
    const dpr = window.devicePixelRatio || 1;
    const rect = canvas.getBoundingClientRect();
    const w = rect.width;
    const h = rect.height;

    if (w === 0 || h === 0) return;

    const bw = Math.round(w * dpr);
    const bh = Math.round(h * dpr);
    if (canvas.width !== bw || canvas.height !== bh) {
      canvas.width = bw;
      canvas.height = bh;
    }
    s.canvasW = w;
    s.canvasH = h;

    const { x: tx, y: ty, k } = s.transform;

    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    ctx.clearRect(0, 0, w, h);

    ctx.save();
    ctx.translate(w / 2 + tx, h / 2 + ty);
    ctx.scale(k, k);

    // Compute hover set for focus effect
    const hoverNode = s.hover;
    const isHoveringAny = hoverNode !== null;
    const hEdges = new Set<SimLink>();
    const hNodes = new Set<number>();
    if (hoverNode) {
      hNodes.add(hoverNode.id);
      for (const link of s.simLinks) {
        const src = link.source as SimNode;
        const tgt = link.target as SimNode;
        if (src.id === hoverNode.id || tgt.id === hoverNode.id) {
          hEdges.add(link);
          hNodes.add(src.id);
          hNodes.add(tgt.id);
        }
      }
    }

    // Draw edges
    for (const link of s.simLinks) {
      const src = link.source as SimNode;
      const tgt = link.target as SimNode;
      if (src.x == null || src.y == null || tgt.x == null || tgt.y == null) continue;
      
      const isHighlighted = hEdges.has(link);
      const opacity = isHoveringAny ? (isHighlighted ? 1 : 0.08) : 1;

      ctx.beginPath();
      ctx.moveTo(src.x, src.y);
      ctx.lineTo(tgt.x, tgt.y);
      
      ctx.strokeStyle = isHighlighted ? EDGE_HIGHLIGHT_COLOR : EDGE_COLOR;
      ctx.lineWidth = (isHighlighted ? 1.5 : 0.8) / Math.max(k, 0.5);
      
      ctx.globalAlpha = opacity;
      ctx.stroke();
    }
    ctx.globalAlpha = 1;

    // Draw nodes
    for (const node of s.simNodes) {
      if (node.x == null || node.y == null) continue;
      const isFocused = hNodes.has(node.id);
      const opacity = isHoveringAny ? (isFocused ? 1 : 0.15) : 1;
      const color = NODE_PALETTE[node.colorIndex % NODE_PALETTE.length];

      ctx.globalAlpha = opacity;

      ctx.beginPath();
      ctx.arc(node.x, node.y, node.radius, 0, Math.PI * 2);
      ctx.fillStyle = color;
      ctx.fill();

      // Clean, subtle outline for structural definition
      ctx.lineWidth = 1 / Math.max(k, 0.5);
      ctx.strokeStyle = "rgba(255,255,255,0.15)";
      ctx.stroke();
    }
    ctx.globalAlpha = 1;

    // Draw labels
    const totalNodes = s.simNodes.length;
    const showAll = totalNodes <= 40 || k >= 1.5;
    const fontSize = Math.max(9, 10 / k);
    ctx.font = `400 ${fontSize}px 'Space Mono', ui-monospace, monospace`;
    ctx.textAlign = "center";
    ctx.textBaseline = "top";
    
    for (const node of s.simNodes) {
      if (node.x == null || node.y == null) continue;
      const isFocused = hNodes.has(node.id);
      const show = isFocused || (showAll && (!isHoveringAny || isFocused)) || (node.degree >= 4 && k >= 0.6 && !isHoveringAny);
      if (!show) continue;

      const labelY = node.y + node.radius + 5;
      const textWidth = ctx.measureText(node.label).width;

      // Restrained background pill for legibility
      ctx.fillStyle = "rgba(11, 15, 20, 0.85)";
      ctx.beginPath();
      ctx.roundRect(node.x - textWidth / 2 - 4, labelY - 2, textWidth + 8, fontSize + 4, 3);
      ctx.fill();

      // Subtle border for the pill
      ctx.strokeStyle = "rgba(122,144,170,0.2)";
      ctx.lineWidth = 1 / Math.max(k, 0.5);
      ctx.stroke();

      ctx.fillStyle = isFocused ? LABEL_COLOR : "#aec0d2";
      ctx.fillText(node.label, node.x, labelY);
    }

    ctx.restore();

    s.frame = requestAnimationFrame(renderFrame);
  }, []);

  // Start render loop
  useEffect(() => {
    stateRef.current.frame = requestAnimationFrame(renderFrame);
    return () => cancelAnimationFrame(stateRef.current.frame);
  }, [renderFrame]);

  // Coordinate conversion
  const toSim = useCallback((clientX: number, clientY: number) => {
    const canvas = canvasRef.current;
    if (!canvas) return { sx: 0, sy: 0 };
    const rect = canvas.getBoundingClientRect();
    const s = stateRef.current;
    const sx = (clientX - rect.left - rect.width / 2 - s.transform.x) / s.transform.k;
    const sy = (clientY - rect.top - rect.height / 2 - s.transform.y) / s.transform.k;
    return { sx, sy };
  }, []);

  const hitTest = useCallback((sx: number, sy: number): SimNode | null => {
    const nodes = stateRef.current.simNodes;
    for (let i = nodes.length - 1; i >= 0; i--) {
      const n = nodes[i];
      if (n.x == null || n.y == null) continue;
      const dx = sx - n.x;
      const dy = sy - n.y;
      if (dx * dx + dy * dy <= (n.radius + 5) * (n.radius + 5)) return n;
    }
    return null;
  }, []);

  // Attach native event handlers
  useEffect(() => {
    const canvas = canvasRef.current;
    if (!canvas) return;
    const s = stateRef.current;

    const onWheel = (e: WheelEvent) => {
      e.preventDefault();
      e.stopPropagation();
      const factor = e.deltaY > 0 ? 0.92 : 1.08;
      s.transform.k = clamp(s.transform.k * factor, 0.1, 8);
    };

    const onMouseDown = (e: MouseEvent) => {
      e.stopPropagation();
      const { sx, sy } = toSim(e.clientX, e.clientY);
      const hit = hitTest(sx, sy);
      if (hit) {
        s.drag = hit;
        s.dragOffset = { x: sx - (hit.x || 0), y: sy - (hit.y || 0) };
        hit.fx = hit.x;
        hit.fy = hit.y;
        canvas.style.cursor = "grabbing";
      } else {
        s.pan = {
          startX: e.clientX,
          startY: e.clientY,
          originX: s.transform.x,
          originY: s.transform.y,
        };
        canvas.style.cursor = "grabbing";
      }
    };

    const onMouseMove = (e: MouseEvent) => {
      const { sx, sy } = toSim(e.clientX, e.clientY);
      if (s.drag) {
        const nx = sx - s.dragOffset.x;
        const ny = sy - s.dragOffset.y;
        s.drag.fx = nx;
        s.drag.fy = ny;
        s.drag.x = nx;
        s.drag.y = ny;
        return;
      }
      if (s.pan) {
        s.transform.x = s.pan.originX + (e.clientX - s.pan.startX);
        s.transform.y = s.pan.originY + (e.clientY - s.pan.startY);
        return;
      }
      const hit = hitTest(sx, sy);
      if (s.hover?.id !== hit?.id) {
        s.hover = hit;
      }
      canvas.style.cursor = hit ? "pointer" : "default";
    };

    const onMouseUp = () => {
      if (s.drag) {
        // Keep node pinned where it was dropped
        s.drag = null;
      }
      s.pan = null;
      canvas.style.cursor = s.hover ? "pointer" : "default";
    };

    canvas.addEventListener("wheel", onWheel, { passive: false });
    canvas.addEventListener("mousedown", onMouseDown);
    canvas.addEventListener("mousemove", onMouseMove);
    canvas.addEventListener("mouseup", onMouseUp);
    canvas.addEventListener("mouseleave", onMouseUp);

    return () => {
      canvas.removeEventListener("wheel", onWheel);
      canvas.removeEventListener("mousedown", onMouseDown);
      canvas.removeEventListener("mousemove", onMouseMove);
      canvas.removeEventListener("mouseup", onMouseUp);
      canvas.removeEventListener("mouseleave", onMouseUp);
    };
  }, [toSim, hitTest]);

  if (!nodes.length) {
    return null;
  }

  return (
    <div ref={containerRef} className="relative h-full w-full overflow-hidden bg-transparent">
      <canvas
        ref={canvasRef}
        className="absolute inset-0 h-full w-full outline-none"
        style={{ background: "transparent" }}
      />
    </div>
  );
}