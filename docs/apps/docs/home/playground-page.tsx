"use client";

import { CypherPlayground } from "./playground";

export function PlaygroundPage({ isEn, locale }: { isEn: boolean; locale: string }) {
  return (
    <div className="flex h-screen w-screen flex-col bg-[#0b0f14] text-[#e7edf5] supports-[height:100dvh]:h-dvh">
      <CypherPlayground isEn={isEn} homeLink={`/${locale}`} />
    </div>
  );
}
