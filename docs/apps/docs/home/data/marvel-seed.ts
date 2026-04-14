// Marvel Universe character co-appearance network
// ~80 heroes, ~250 edges

type Edge = [string, string, number];

const heroes: [string, string][] = [
  // Avengers core
  ['SPIDER-MAN', 'Spider-Man'], ['IRON-MAN', 'Iron Man'], ['CAPTAIN-AMERICA', 'Captain America'],
  ['THOR', 'Thor'], ['HULK', 'Hulk'], ['BLACK-WIDOW', 'Black Widow'],
  ['HAWKEYE', 'Hawkeye'], ['VISION', 'Vision'], ['SCARLET-WITCH', 'Scarlet Witch'],
  ['ANT-MAN', 'Ant-Man'], ['WASP', 'Wasp'],
  // Extended Avengers
  ['BLACK-PANTHER', 'Black Panther'], ['DOCTOR-STRANGE', 'Doctor Strange'],
  ['CAPTAIN-MARVEL', 'Captain Marvel'], ['WAR-MACHINE', 'War Machine'],
  ['FALCON', 'Falcon'], ['WINTER-SOLDIER', 'Winter Soldier'], ['QUICKSILVER', 'Quicksilver'],
  // X-Men
  ['WOLVERINE', 'Wolverine'], ['STORM', 'Storm'], ['CYCLOPS', 'Cyclops'],
  ['JEAN-GREY', 'Jean Grey'], ['BEAST', 'Beast'], ['NIGHTCRAWLER', 'Nightcrawler'],
  ['COLOSSUS', 'Colossus'], ['ROGUE', 'Rogue'], ['GAMBIT', 'Gambit'],
  ['ICEMAN', 'Iceman'], ['ANGEL', 'Angel'], ['PROFESSOR-X', 'Professor X'],
  ['MAGNETO', 'Magneto'], ['MYSTIQUE', 'Mystique'], ['PSYLOCKE', 'Psylocke'],
  ['JUBILEE', 'Jubilee'], ['KITTY-PRYDE', 'Kitty Pryde'], ['EMMA-FROST', 'Emma Frost'],
  ['BISHOP', 'Bishop'], ['CABLE', 'Cable'],
  // Fantastic Four
  ['MISTER-FANTASTIC', 'Mister Fantastic'], ['INVISIBLE-WOMAN', 'Invisible Woman'],
  ['HUMAN-TORCH', 'Human Torch'], ['THING', 'Thing'],
  // Guardians
  ['STAR-LORD', 'Star-Lord'], ['GAMORA', 'Gamora'], ['DRAX', 'Drax'],
  ['ROCKET-RACCOON', 'Rocket Raccoon'], ['GROOT', 'Groot'],
  ['MANTIS', 'Mantis'], ['NEBULA', 'Nebula'],
  // Defenders
  ['DAREDEVIL', 'Daredevil'], ['LUKE-CAGE', 'Luke Cage'],
  ['IRON-FIST', 'Iron Fist'], ['JESSICA-JONES', 'Jessica Jones'],
  // Others
  ['DEADPOOL', 'Deadpool'], ['PUNISHER', 'Punisher'], ['SILVER-SURFER', 'Silver Surfer'],
  ['NAMOR', 'Namor'], ['MOON-KNIGHT', 'Moon Knight'], ['SHE-HULK', 'She-Hulk'],
  ['MS-MARVEL', 'Ms. Marvel'], ['SHANG-CHI', 'Shang-Chi'], ['BLADE', 'Blade'],
  ['GHOST-RIDER', 'Ghost Rider'],
  // Villains/Antiheroes
  ['LOKI', 'Loki'], ['THANOS', 'Thanos'], ['VENOM', 'Venom'],
  ['DOCTOR-DOOM', 'Doctor Doom'], ['GREEN-GOBLIN', 'Green Goblin'],
  ['KINGPIN', 'Kingpin'], ['ULTRON', 'Ultron'], ['RED-SKULL', 'Red Skull'],
  ['GALACTUS', 'Galactus'], ['KANG', 'Kang'],
];

const edges: Edge[] = [
  // Very strong (20+)
  ['SPIDER-MAN', 'IRON-MAN', 25],
  ['CAPTAIN-AMERICA', 'IRON-MAN', 24],
  ['THOR', 'IRON-MAN', 22],
  ['WOLVERINE', 'CYCLOPS', 23],
  ['WOLVERINE', 'STORM', 21],
  ['MISTER-FANTASTIC', 'INVISIBLE-WOMAN', 25],
  ['CAPTAIN-AMERICA', 'WINTER-SOLDIER', 20],
  ['CAPTAIN-AMERICA', 'THOR', 21],
  ['WOLVERINE', 'JEAN-GREY', 20],
  ['CYCLOPS', 'JEAN-GREY', 22],
  // Strong Avengers (15-19)
  ['SPIDER-MAN', 'CAPTAIN-AMERICA', 18],
  ['IRON-MAN', 'WAR-MACHINE', 18],
  ['IRON-MAN', 'HULK', 17],
  ['THOR', 'HULK', 16],
  ['IRON-MAN', 'BLACK-WIDOW', 15],
  ['CAPTAIN-AMERICA', 'BLACK-WIDOW', 16],
  ['CAPTAIN-AMERICA', 'FALCON', 17],
  ['HAWKEYE', 'BLACK-WIDOW', 16],
  ['VISION', 'SCARLET-WITCH', 18],
  ['ANT-MAN', 'WASP', 17],
  ['SPIDER-MAN', 'THOR', 15],
  ['THOR', 'LOKI', 19],
  ['CAPTAIN-AMERICA', 'IRON-MAN', 24], // dup removed below
  // Strong X-Men (10-19)
  ['WOLVERINE', 'PROFESSOR-X', 16],
  ['CYCLOPS', 'PROFESSOR-X', 15],
  ['STORM', 'PROFESSOR-X', 14],
  ['WOLVERINE', 'ROGUE', 14],
  ['STORM', 'CYCLOPS', 15],
  ['JEAN-GREY', 'PROFESSOR-X', 13],
  ['BEAST', 'PROFESSOR-X', 12],
  ['WOLVERINE', 'NIGHTCRAWLER', 12],
  ['WOLVERINE', 'COLOSSUS', 13],
  ['CYCLOPS', 'EMMA-FROST', 14],
  ['GAMBIT', 'ROGUE', 16],
  ['KITTY-PRYDE', 'COLOSSUS', 13],
  ['WOLVERINE', 'BEAST', 11],
  ['STORM', 'JEAN-GREY', 11],
  ['MAGNETO', 'PROFESSOR-X', 15],
  ['MAGNETO', 'MYSTIQUE', 12],
  ['CABLE', 'DEADPOOL', 14],
  ['PSYLOCKE', 'WOLVERINE', 10],
  ['ICEMAN', 'BEAST', 10],
  ['ICEMAN', 'ANGEL', 10],
  ['NIGHTCRAWLER', 'STORM', 10],
  // Strong FF (10-19)
  ['MISTER-FANTASTIC', 'THING', 19],
  ['MISTER-FANTASTIC', 'HUMAN-TORCH', 18],
  ['INVISIBLE-WOMAN', 'THING', 17],
  ['INVISIBLE-WOMAN', 'HUMAN-TORCH', 16],
  ['HUMAN-TORCH', 'THING', 18],
  ['THING', 'HULK', 10],
  // Strong Guardians (10-19)
  ['STAR-LORD', 'GAMORA', 17],
  ['STAR-LORD', 'DRAX', 15],
  ['STAR-LORD', 'ROCKET-RACCOON', 16],
  ['ROCKET-RACCOON', 'GROOT', 19],
  ['GAMORA', 'DRAX', 13],
  ['GAMORA', 'NEBULA', 14],
  ['STAR-LORD', 'GROOT', 12],
  ['MANTIS', 'DRAX', 11],
  ['MANTIS', 'STAR-LORD', 10],
  // Strong Defenders (10-15)
  ['DAREDEVIL', 'LUKE-CAGE', 12],
  ['DAREDEVIL', 'IRON-FIST', 11],
  ['LUKE-CAGE', 'IRON-FIST', 14],
  ['LUKE-CAGE', 'JESSICA-JONES', 15],
  ['DAREDEVIL', 'JESSICA-JONES', 10],
  ['DAREDEVIL', 'KINGPIN', 13],
  // Medium cross-team (5-9)
  ['SPIDER-MAN', 'DOCTOR-STRANGE', 9],
  ['SPIDER-MAN', 'BLACK-PANTHER', 8],
  ['SPIDER-MAN', 'WOLVERINE', 8],
  ['SPIDER-MAN', 'DAREDEVIL', 9],
  ['SPIDER-MAN', 'VENOM', 9],
  ['SPIDER-MAN', 'GREEN-GOBLIN', 9],
  ['IRON-MAN', 'DOCTOR-STRANGE', 8],
  ['IRON-MAN', 'BLACK-PANTHER', 8],
  ['IRON-MAN', 'SPIDER-MAN', 25], // dup
  ['IRON-MAN', 'CAPTAIN-MARVEL', 7],
  ['CAPTAIN-AMERICA', 'BLACK-PANTHER', 9],
  ['CAPTAIN-AMERICA', 'DOCTOR-STRANGE', 6],
  ['CAPTAIN-AMERICA', 'ANT-MAN', 7],
  ['THOR', 'DOCTOR-STRANGE', 7],
  ['THOR', 'CAPTAIN-MARVEL', 7],
  ['THOR', 'ROCKET-RACCOON', 6],
  ['HULK', 'DOCTOR-STRANGE', 6],
  ['HULK', 'BLACK-WIDOW', 8],
  ['HULK', 'WOLVERINE', 7],
  ['HULK', 'THING', 8],
  ['HULK', 'SHE-HULK', 9],
  ['BLACK-WIDOW', 'WINTER-SOLDIER', 7],
  ['BLACK-PANTHER', 'DOCTOR-STRANGE', 5],
  ['BLACK-PANTHER', 'CAPTAIN-MARVEL', 5],
  ['BLACK-PANTHER', 'STORM', 7],
  ['WOLVERINE', 'HULK', 7], // dup
  ['WOLVERINE', 'CAPTAIN-AMERICA', 7],
  ['WOLVERINE', 'SPIDER-MAN', 8], // dup
  ['WOLVERINE', 'DEADPOOL', 9],
  ['DOCTOR-STRANGE', 'SCARLET-WITCH', 7],
  ['MISTER-FANTASTIC', 'DOCTOR-DOOM', 9],
  ['MISTER-FANTASTIC', 'BLACK-PANTHER', 6],
  ['MISTER-FANTASTIC', 'NAMOR', 7],
  ['INVISIBLE-WOMAN', 'NAMOR', 6],
  ['SILVER-SURFER', 'GALACTUS', 9],
  ['SILVER-SURFER', 'MISTER-FANTASTIC', 7],
  ['SILVER-SURFER', 'THING', 5],
  ['THANOS', 'IRON-MAN', 8],
  ['THANOS', 'THOR', 7],
  ['THANOS', 'CAPTAIN-AMERICA', 6],
  ['THANOS', 'GAMORA', 8],
  ['THANOS', 'NEBULA', 7],
  ['THANOS', 'DOCTOR-STRANGE', 6],
  ['THANOS', 'STAR-LORD', 5],
  ['ULTRON', 'IRON-MAN', 8],
  ['ULTRON', 'VISION', 7],
  ['ULTRON', 'CAPTAIN-AMERICA', 5],
  ['ULTRON', 'SCARLET-WITCH', 6],
  ['KANG', 'IRON-MAN', 5],
  ['KANG', 'CAPTAIN-AMERICA', 5],
  ['KANG', 'MISTER-FANTASTIC', 5],
  ['LOKI', 'IRON-MAN', 6],
  ['LOKI', 'CAPTAIN-AMERICA', 5],
  ['RED-SKULL', 'CAPTAIN-AMERICA', 8],
  ['RED-SKULL', 'WINTER-SOLDIER', 5],
  ['DOCTOR-DOOM', 'IRON-MAN', 6],
  ['DOCTOR-DOOM', 'THING', 7],
  ['DOCTOR-DOOM', 'BLACK-PANTHER', 5],
  ['VENOM', 'IRON-MAN', 5],
  ['KINGPIN', 'SPIDER-MAN', 7],
  ['KINGPIN', 'PUNISHER', 6],
  ['GREEN-GOBLIN', 'IRON-MAN', 5],
  ['GALACTUS', 'MISTER-FANTASTIC', 7],
  ['GALACTUS', 'THOR', 5],
  // Medium team-internal (5-9)
  ['SCARLET-WITCH', 'QUICKSILVER', 9],
  ['SCARLET-WITCH', 'HAWKEYE', 6],
  ['VISION', 'IRON-MAN', 7],
  ['VISION', 'THOR', 5],
  ['FALCON', 'WINTER-SOLDIER', 8],
  ['FALCON', 'BLACK-WIDOW', 5],
  ['WAR-MACHINE', 'CAPTAIN-AMERICA', 6],
  ['WAR-MACHINE', 'BLACK-WIDOW', 5],
  ['ANT-MAN', 'IRON-MAN', 6],
  ['ANT-MAN', 'CAPTAIN-AMERICA', 7],
  ['WASP', 'CAPTAIN-AMERICA', 5],
  ['BLACK-PANTHER', 'FALCON', 6],
  ['CAPTAIN-MARVEL', 'BLACK-WIDOW', 5],
  ['CAPTAIN-MARVEL', 'SCARLET-WITCH', 5],
  ['ROGUE', 'CYCLOPS', 8],
  ['ROGUE', 'STORM', 7],
  ['GAMBIT', 'WOLVERINE', 8],
  ['GAMBIT', 'STORM', 6],
  ['COLOSSUS', 'CYCLOPS', 8],
  ['COLOSSUS', 'STORM', 7],
  ['NIGHTCRAWLER', 'COLOSSUS', 9],
  ['NIGHTCRAWLER', 'CYCLOPS', 7],
  ['BEAST', 'CYCLOPS', 9],
  ['BEAST', 'JEAN-GREY', 8],
  ['ANGEL', 'CYCLOPS', 8],
  ['ANGEL', 'JEAN-GREY', 7],
  ['JUBILEE', 'WOLVERINE', 8],
  ['JUBILEE', 'STORM', 5],
  ['KITTY-PRYDE', 'WOLVERINE', 9],
  ['KITTY-PRYDE', 'STORM', 6],
  ['EMMA-FROST', 'WOLVERINE', 7],
  ['EMMA-FROST', 'MAGNETO', 6],
  ['BISHOP', 'STORM', 6],
  ['BISHOP', 'WOLVERINE', 5],
  ['CABLE', 'WOLVERINE', 7],
  ['CABLE', 'CYCLOPS', 6],
  ['MYSTIQUE', 'WOLVERINE', 6],
  ['MYSTIQUE', 'ROGUE', 7],
  ['MAGNETO', 'WOLVERINE', 8],
  ['MAGNETO', 'CYCLOPS', 6],
  ['MAGNETO', 'SCARLET-WITCH', 7],
  ['MAGNETO', 'QUICKSILVER', 7],
  ['PROFESSOR-X', 'MAGNETO', 15], // dup
  ['PSYLOCKE', 'CYCLOPS', 6],
  ['PSYLOCKE', 'JEAN-GREY', 5],
  ['ROCKET-RACCOON', 'DRAX', 11],
  ['ROCKET-RACCOON', 'GAMORA', 10],
  ['GROOT', 'DRAX', 9],
  ['GROOT', 'GAMORA', 8],
  ['NEBULA', 'DRAX', 6],
  ['NEBULA', 'ROCKET-RACCOON', 7],
  ['MANTIS', 'GAMORA', 7],
  ['MANTIS', 'ROCKET-RACCOON', 5],
  ['IRON-FIST', 'JESSICA-JONES', 8],
  ['DAREDEVIL', 'PUNISHER', 8],
  ['DAREDEVIL', 'SPIDER-MAN', 9], // dup
  ['LUKE-CAGE', 'DAREDEVIL', 12], // dup
  // Low (1-4)
  ['DEADPOOL', 'SPIDER-MAN', 4],
  ['DEADPOOL', 'WOLVERINE', 9], // dup
  ['DEADPOOL', 'IRON-MAN', 3],
  ['DEADPOOL', 'COLOSSUS', 4],
  ['PUNISHER', 'SPIDER-MAN', 4],
  ['PUNISHER', 'CAPTAIN-AMERICA', 3],
  ['MOON-KNIGHT', 'SPIDER-MAN', 3],
  ['MOON-KNIGHT', 'CAPTAIN-AMERICA', 2],
  ['MOON-KNIGHT', 'WOLVERINE', 3],
  ['SHE-HULK', 'IRON-MAN', 4],
  ['SHE-HULK', 'CAPTAIN-AMERICA', 3],
  ['SHE-HULK', 'THING', 4],
  ['SHE-HULK', 'MISTER-FANTASTIC', 3],
  ['MS-MARVEL', 'CAPTAIN-MARVEL', 4],
  ['MS-MARVEL', 'IRON-MAN', 3],
  ['MS-MARVEL', 'SPIDER-MAN', 3],
  ['SHANG-CHI', 'IRON-MAN', 3],
  ['SHANG-CHI', 'CAPTAIN-AMERICA', 2],
  ['SHANG-CHI', 'SPIDER-MAN', 2],
  ['BLADE', 'WOLVERINE', 3],
  ['BLADE', 'SPIDER-MAN', 2],
  ['BLADE', 'DOCTOR-STRANGE', 3],
  ['GHOST-RIDER', 'WOLVERINE', 3],
  ['GHOST-RIDER', 'SPIDER-MAN', 2],
  ['GHOST-RIDER', 'DOCTOR-STRANGE', 3],
  ['GHOST-RIDER', 'BLADE', 4],
  ['NAMOR', 'CAPTAIN-AMERICA', 4],
  ['NAMOR', 'BLACK-PANTHER', 3],
  ['NAMOR', 'DOCTOR-DOOM', 4],
  ['NAMOR', 'THING', 3],
  ['VENOM', 'CAPTAIN-AMERICA', 2],
  ['VENOM', 'WOLVERINE', 2],
  ['GREEN-GOBLIN', 'CAPTAIN-AMERICA', 2],
  ['KANG', 'THOR', 3],
  ['KANG', 'HULK', 2],
  ['GALACTUS', 'SILVER-SURFER', 9], // dup
  ['GALACTUS', 'DOCTOR-DOOM', 3],
  ['RED-SKULL', 'IRON-MAN', 3],
  ['LOKI', 'DOCTOR-STRANGE', 4],
  ['LOKI', 'HULK', 4],
  ['ULTRON', 'THOR', 4],
  ['ULTRON', 'HAWKEYE', 3],
  ['QUICKSILVER', 'HAWKEYE', 4],
  ['QUICKSILVER', 'VISION', 3],
  ['STAR-LORD', 'THOR', 4],
  ['STAR-LORD', 'IRON-MAN', 3],
  ['JESSICA-JONES', 'SPIDER-MAN', 2],
  ['JESSICA-JONES', 'CAPTAIN-AMERICA', 2],
  ['IRON-FIST', 'WOLVERINE', 3],
  ['IRON-FIST', 'SPIDER-MAN', 3],
  ['LUKE-CAGE', 'SPIDER-MAN', 3],
  ['LUKE-CAGE', 'CAPTAIN-AMERICA', 3],
];

// Deduplicate edges (keep first occurrence)
function deduplicateEdges(edgeList: Edge[]): Edge[] {
  const seen = new Set<string>();
  const result: Edge[] = [];
  for (const [a, b, w] of edgeList) {
    const key = [a, b].sort().join('--');
    if (!seen.has(key)) {
      seen.add(key);
      result.push([a, b, w]);
    }
  }
  return result;
}

function buildNodeCreates(heroList: [string, string][]): string[] {
  return heroList.map(
    ([id, name]) => `CREATE (:Hero {id: '${id}', name: '${name.replace(/'/g, "\\'")}'})`,
  );
}

function buildEdgeCreates(edgeList: Edge[]): string[] {
  return edgeList.map(
    ([a, b, w]) =>
      `MATCH (a:Hero {id: '${a}'}), (b:Hero {id: '${b}'}) CREATE (a)-[:ALLIES_WITH {weight: ${w}}]->(b)`,
  );
}

const dedupedEdges = deduplicateEdges(edges);

export const MARVEL_SEED_QUERIES: string[] = [
  ...buildNodeCreates(heroes),
  ...buildEdgeCreates(dedupedEdges),
];

export const MARVEL_EXAMPLE_QUERIES: { label: string; query: string }[] = [
  {
    label: 'All heroes',
    query: 'MATCH (n:Hero) RETURN n.name ORDER BY n.name',
  },
  {
    label: "Spider-Man's network",
    query:
      "MATCH (s:Hero {id: 'SPIDER-MAN'})-[r:ALLIES_WITH]-(other) RETURN other.name, r.weight ORDER BY r.weight DESC",
  },
  {
    label: 'Strongest alliances',
    query:
      'MATCH (a)-[r:ALLIES_WITH]->(b) WHERE r.weight >= 15 RETURN a.name, b.name, r.weight ORDER BY r.weight DESC',
  },
  {
    label: 'Most connected heroes',
    query:
      'MATCH (n:Hero)-[r:ALLIES_WITH]-() RETURN n.name, count(r) AS connections ORDER BY connections DESC LIMIT 10',
  },
  {
    label: 'Avengers circle',
    query:
      "MATCH (a:Hero)-[r:ALLIES_WITH]-(other) WHERE a.id IN ['IRON-MAN', 'CAPTAIN-AMERICA', 'THOR', 'SPIDER-MAN'] RETURN a.name, other.name, r.weight ORDER BY r.weight DESC",
  },
];
