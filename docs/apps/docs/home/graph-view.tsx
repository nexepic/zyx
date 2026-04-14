"use client";

import { useCallback, useEffect, useRef, useState } from "react";
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
  score?: number;
  highlighted?: boolean;
  props?: Record<string, unknown>;
  nodeLabel?: string;
}

export interface GraphEdge {
  id: number;
  sourceId: number;
  targetId: number;
  type: string;
  highlighted?: boolean;
}

interface SimNode extends SimulationNodeDatum {
  id: number;
  label: string;
  degree: number;
  score: number;
  highlighted: boolean;
  radius: number;
  colorIndex: number;
  props?: Record<string, unknown>;
  nodeLabel?: string;
}

interface SelectedNodeInfo {
  id: number;
  label: string;
  nodeLabel?: string;
  degree: number;
  score: number;
  highlighted: boolean;
  props?: Record<string, unknown>;
  connections: { id: number; label: string; type: string; direction: "out" | "in" }[];
}

interface SimLink extends SimulationLinkDatum<SimNode> {
  type: string;
  highlighted: boolean;
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
const PATH_NODE_COLOR = "#d4a843";
const PATH_EDGE_COLOR = "rgba(212,168,67,0.85)";
const PATH_GLOW_COLOR = "rgba(212,168,67,0.3)";

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
  const [selected, setSelected] = useState<SelectedNodeInfo | null>(null);

  const stateRef = useRef({
    simNodes: [] as SimNode[],
    simLinks: [] as SimLink[],
    transform: { x: 0, y: 0, k: 1 },
    drag: null as SimNode | null,
    dragOffset: { x: 0, y: 0 },
    dragMoved: false,
    pan: null as { startX: number; startY: number; originX: number; originY: number } | null,
    panMoved: false,
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

    // Check if any nodes have GDS scores for score-based sizing
    const hasScores = nodes.some((n) => n.score !== undefined);
    let maxScore = 0;
    let minScore = Infinity;
    if (hasScores) {
      for (const n of nodes) {
        if (n.score !== undefined) {
          if (n.score > maxScore) maxScore = n.score;
          if (n.score < minScore) minScore = n.score;
        }
      }
    }
    const scoreRange = maxScore - minScore || 1;

    const sNodes: SimNode[] = nodes.map((n) => {
      const deg = degreeMap.get(n.id) || 0;
      const score = n.score ?? 0;
      // When scores are available, size by score; otherwise by degree
      const radius = hasScores
        ? 4 + 12 * ((score - minScore) / scoreRange)
        : 4 + 8 * (deg / maxDegree);
      return {
        id: n.id,
        label: n.label,
        degree: deg,
        score,
        highlighted: n.highlighted ?? false,
        radius: n.highlighted ? Math.max(radius, 8) : radius,
        colorIndex: 0,
        props: n.props,
        nodeLabel: n.nodeLabel,
      };
    });

    const nodeIndex = new Map(sNodes.map((n) => [n.id, n]));
    const sLinks: SimLink[] = [];
    for (const e of edges) {
      const src = nodeIndex.get(e.sourceId);
      const tgt = nodeIndex.get(e.targetId);
      if (src && tgt) {
        sLinks.push({ source: src, target: tgt, type: e.type, highlighted: e.highlighted ?? false });
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

    // Check if any path highlighting is active
    const hasPathHighlight = s.simLinks.some((l) => l.highlighted) || s.simNodes.some((n) => n.highlighted);

    // Draw edges with arrowheads
    const arrowSize = 5 / Math.max(k, 0.3);
    for (const link of s.simLinks) {
      const src = link.source as SimNode;
      const tgt = link.target as SimNode;
      if (src.x == null || src.y == null || tgt.x == null || tgt.y == null) continue;

      const isHoverHL = hEdges.has(link);
      const isPathHL = link.highlighted;
      const dimmed = hasPathHighlight && !isPathHL && !isHoverHL;
      const opacity = isHoveringAny
        ? (isHoverHL ? 1 : 0.08)
        : dimmed ? 0.08 : 1;

      const dx = tgt.x - src.x;
      const dy = tgt.y - src.y;
      const dist = Math.sqrt(dx * dx + dy * dy);
      if (dist < 1) continue;

      // Unit vector
      const ux = dx / dist;
      const uy = dy / dist;

      // Stop the line at the target node's edge (not center)
      const endX = tgt.x - ux * tgt.radius;
      const endY = tgt.y - uy * tgt.radius;

      const edgeColor = isPathHL ? PATH_EDGE_COLOR : isHoverHL ? EDGE_HIGHLIGHT_COLOR : EDGE_COLOR;
      const lineW = (isPathHL ? 2.5 : isHoverHL ? 1.5 : 0.8) / Math.max(k, 0.5);

      ctx.globalAlpha = opacity;

      // Line
      ctx.beginPath();
      ctx.moveTo(src.x, src.y);
      ctx.lineTo(endX, endY);
      ctx.strokeStyle = edgeColor;
      ctx.lineWidth = lineW;
      ctx.stroke();

      // Arrowhead (triangle at the end)
      const aLen = isPathHL ? arrowSize * 1.4 : isHoverHL ? arrowSize * 1.2 : arrowSize;
      const ax = endX - ux * aLen;
      const ay = endY - uy * aLen;
      const perpX = -uy * aLen * 0.5;
      const perpY = ux * aLen * 0.5;

      ctx.beginPath();
      ctx.moveTo(endX, endY);
      ctx.lineTo(ax + perpX, ay + perpY);
      ctx.lineTo(ax - perpX, ay - perpY);
      ctx.closePath();
      ctx.fillStyle = edgeColor;
      ctx.fill();
    }
    ctx.globalAlpha = 1;

    // Draw nodes
    for (const node of s.simNodes) {
      if (node.x == null || node.y == null) continue;
      const isFocused = hNodes.has(node.id);
      const isPathHL = node.highlighted;
      const dimmed = hasPathHighlight && !isPathHL && !isFocused;
      const opacity = isHoveringAny
        ? (isFocused ? 1 : 0.15)
        : dimmed ? 0.15 : 1;
      const color = isPathHL ? PATH_NODE_COLOR : NODE_PALETTE[node.colorIndex % NODE_PALETTE.length];

      ctx.globalAlpha = opacity;

      // Glow for path-highlighted nodes
      if (isPathHL && !dimmed) {
        ctx.beginPath();
        ctx.arc(node.x, node.y, node.radius + 4, 0, Math.PI * 2);
        ctx.fillStyle = PATH_GLOW_COLOR;
        ctx.fill();
      }

      ctx.beginPath();
      ctx.arc(node.x, node.y, node.radius, 0, Math.PI * 2);
      ctx.fillStyle = color;
      ctx.fill();

      // Clean, subtle outline for structural definition
      ctx.lineWidth = 1 / Math.max(k, 0.5);
      ctx.strokeStyle = isPathHL ? "rgba(212,168,67,0.5)" : "rgba(255,255,255,0.15)";
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
      const isPathHL = node.highlighted;
      const show = isPathHL || isFocused || (showAll && (!isHoveringAny || isFocused)) || (node.degree >= 4 && k >= 0.6 && !isHoveringAny);
      if (!show) continue;

      const labelY = node.y + node.radius + 5;
      const labelText = node.label;
      const scoreText = node.score ? node.score.toFixed(4) : "";
      const showScore = isFocused && scoreText;
      const textWidth = ctx.measureText(labelText).width;
      const pillHeight = showScore ? fontSize * 2 + 6 : fontSize + 4;

      // Restrained background pill for legibility
      ctx.fillStyle = "rgba(11, 15, 20, 0.85)";
      ctx.beginPath();
      ctx.roundRect(node.x - textWidth / 2 - 4, labelY - 2, textWidth + 8, pillHeight, 3);
      ctx.fill();

      // Subtle border for the pill
      ctx.strokeStyle = "rgba(122,144,170,0.2)";
      ctx.lineWidth = 1 / Math.max(k, 0.5);
      ctx.stroke();

      ctx.fillStyle = isFocused ? LABEL_COLOR : "#aec0d2";
      ctx.fillText(labelText, node.x, labelY);

      if (showScore) {
        ctx.fillStyle = "#6b8a9c";
        ctx.fillText(scoreText, node.x, labelY + fontSize + 2);
      }
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

  // Build selected node info from a SimNode
  const buildSelectedInfo = useCallback((node: SimNode): SelectedNodeInfo => {
    const s = stateRef.current;
    const connections: SelectedNodeInfo["connections"] = [];
    for (const link of s.simLinks) {
      const src = link.source as SimNode;
      const tgt = link.target as SimNode;
      if (src.id === node.id) {
        connections.push({ id: tgt.id, label: tgt.label, type: link.type, direction: "out" });
      } else if (tgt.id === node.id) {
        connections.push({ id: src.id, label: src.label, type: link.type, direction: "in" });
      }
    }
    return {
      id: node.id,
      label: node.label,
      nodeLabel: node.nodeLabel,
      degree: node.degree,
      score: node.score,
      highlighted: node.highlighted,
      props: node.props,
      connections,
    };
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
        s.dragMoved = false;
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
        s.panMoved = false;
        canvas.style.cursor = "grabbing";
      }
    };

    const onMouseMove = (e: MouseEvent) => {
      const { sx, sy } = toSim(e.clientX, e.clientY);
      if (s.drag) {
        s.dragMoved = true;
        const nx = sx - s.dragOffset.x;
        const ny = sy - s.dragOffset.y;
        s.drag.fx = nx;
        s.drag.fy = ny;
        s.drag.x = nx;
        s.drag.y = ny;
        return;
      }
      if (s.pan) {
        const dx = e.clientX - s.pan.startX;
        const dy = e.clientY - s.pan.startY;
        if (Math.abs(dx) > 3 || Math.abs(dy) > 3) s.panMoved = true;
        s.transform.x = s.pan.originX + dx;
        s.transform.y = s.pan.originY + dy;
        return;
      }
      const hit = hitTest(sx, sy);
      if (s.hover?.id !== hit?.id) {
        s.hover = hit;
      }
      canvas.style.cursor = hit ? "pointer" : "default";
    };

    const onMouseUp = (e: MouseEvent) => {
      // Detect click: mousedown + mouseup on a node without dragging
      if (s.drag && !s.dragMoved) {
        // Click on a node — select it
        setSelected(buildSelectedInfo(s.drag));
        s.drag.fx = s.drag.x;
        s.drag.fy = s.drag.y;
      } else if (s.pan && !s.panMoved) {
        // Click on empty space — deselect
        setSelected(null);
      }

      if (s.drag) {
        s.drag = null;
      }
      s.pan = null;
      canvas.style.cursor = s.hover ? "pointer" : "default";
    };

    canvas.addEventListener("wheel", onWheel, { passive: false });
    canvas.addEventListener("mousedown", onMouseDown);
    canvas.addEventListener("mousemove", onMouseMove);
    canvas.addEventListener("mouseup", onMouseUp);
    canvas.addEventListener("mouseleave", () => {
      if (s.drag) s.drag = null;
      s.pan = null;
      canvas.style.cursor = "default";
    });

    return () => {
      canvas.removeEventListener("wheel", onWheel);
      canvas.removeEventListener("mousedown", onMouseDown);
      canvas.removeEventListener("mousemove", onMouseMove);
      canvas.removeEventListener("mouseup", onMouseUp);
    };
  }, [toSim, hitTest, buildSelectedInfo]);

  // Clear selection when graph data changes
  useEffect(() => {
    setSelected(null);
  }, [nodes, edges]);

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

      {/* Node detail panel — bottom-left overlay */}
      {selected && (
        <div className="absolute bottom-4 left-4 z-20 w-[280px] max-h-[60%] overflow-y-auto rounded-lg border border-[rgba(122,144,170,0.25)] bg-[rgba(11,15,20,0.92)] backdrop-blur-md shadow-xl [scrollbar-width:thin] [scrollbar-color:rgba(122,144,170,0.2)_transparent]">
          {/* Header */}
          <div className="sticky top-0 z-10 flex items-center justify-between border-b border-[rgba(122,144,170,0.15)] bg-[rgba(11,15,20,0.95)] px-4 py-3">
            <div className="flex items-center gap-2 min-w-0">
              <div className="h-2.5 w-2.5 shrink-0 rounded-full" style={{ background: selected.highlighted ? PATH_NODE_COLOR : "#6b829c" }} />
              <span className="truncate text-[0.8rem] font-medium text-[#e7edf5]">{selected.label}</span>
            </div>
            <button
              onClick={() => setSelected(null)}
              className="flex h-5 w-5 shrink-0 items-center justify-center rounded text-[#566b82] transition-colors hover:text-[#8a9bb0]"
            >
              <svg width="10" height="10" viewBox="0 0 10 10" fill="none" stroke="currentColor" strokeWidth="1.5" strokeLinecap="round">
                <path d="M2 2L8 8M8 2L2 8" />
              </svg>
            </button>
          </div>

          <div className="px-4 py-3 space-y-3">
            {/* Identity */}
            <div>
              <p className="m-0 mb-1.5 text-[0.55rem] font-medium uppercase tracking-[0.15em] text-[#566b82]">Identity</p>
              <div className="space-y-1">
                <div className="flex items-center justify-between">
                  <span className="text-[0.65rem] text-[#6b7f94]">ID</span>
                  <span className="font-['Space_Mono','Ubuntu_Mono',monospace] text-[0.7rem] text-[#aec0d2]">{selected.id}</span>
                </div>
                {selected.nodeLabel && (
                  <div className="flex items-center justify-between">
                    <span className="text-[0.65rem] text-[#6b7f94]">Label</span>
                    <span className="rounded-full border border-[rgba(107,138,118,0.35)] bg-[rgba(107,138,118,0.1)] px-2 py-0.5 text-[0.6rem] font-medium text-[#8aad96]">
                      {selected.nodeLabel}
                    </span>
                  </div>
                )}
                <div className="flex items-center justify-between">
                  <span className="text-[0.65rem] text-[#6b7f94]">Degree</span>
                  <span className="font-['Space_Mono','Ubuntu_Mono',monospace] text-[0.7rem] text-[#aec0d2]">{selected.degree}</span>
                </div>
                {selected.score !== 0 && (
                  <div className="flex items-center justify-between">
                    <span className="text-[0.65rem] text-[#6b7f94]">Score</span>
                    <span className="font-['Space_Mono','Ubuntu_Mono',monospace] text-[0.7rem] text-[#a0c4d8]">{selected.score.toFixed(6)}</span>
                  </div>
                )}
              </div>
            </div>

            {/* Properties */}
            {selected.props && Object.keys(selected.props).length > 0 && (
              <div>
                <div className="mb-1.5 border-t border-[rgba(122,144,170,0.08)]" />
                <p className="m-0 mb-1.5 text-[0.55rem] font-medium uppercase tracking-[0.15em] text-[#566b82]">Properties</p>
                <div className="space-y-1">
                  {Object.entries(selected.props).map(([key, val]) => (
                    <div key={key} className="flex items-start justify-between gap-3">
                      <span className="shrink-0 text-[0.65rem] text-[#6b7f94]">{key}</span>
                      <span className="break-all text-right font-['Space_Mono','Ubuntu_Mono',monospace] text-[0.65rem] text-[#aec0d2]">
                        {typeof val === "object" ? JSON.stringify(val) : String(val)}
                      </span>
                    </div>
                  ))}
                </div>
              </div>
            )}

            {/* Connections */}
            {selected.connections.length > 0 && (
              <div>
                <div className="mb-1.5 border-t border-[rgba(122,144,170,0.08)]" />
                <p className="m-0 mb-1.5 text-[0.55rem] font-medium uppercase tracking-[0.15em] text-[#566b82]">
                  Connections ({selected.connections.length})
                </p>
                <div className="space-y-0.5 max-h-[120px] overflow-y-auto [scrollbar-width:thin] [scrollbar-color:rgba(122,144,170,0.2)_transparent]">
                  {selected.connections.map((c, i) => (
                    <div key={i} className="flex items-center gap-1.5 text-[0.6rem]">
                      <span className="text-[#566b82]">{c.direction === "out" ? "\u2192" : "\u2190"}</span>
                      <span className="rounded border border-[rgba(163,141,93,0.25)] bg-[rgba(163,141,93,0.06)] px-1 py-px text-[0.55rem] font-['Space_Mono','Ubuntu_Mono',monospace] text-[#b8a473]">
                        {c.type}
                      </span>
                      <span className="truncate text-[#8a9bb0]">{c.label}</span>
                    </div>
                  ))}
                </div>
              </div>
            )}
          </div>
        </div>
      )}
    </div>
  );
}