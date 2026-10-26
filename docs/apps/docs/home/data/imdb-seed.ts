// IMDb Movie Database - Top 400 highest-rated movies
// Generated from IMDb_All_Genres_etf_clean1.csv
// Graph model: Person -[:DIRECTED]-> Movie, Person -[:ACTED_IN]-> Movie, Movie -[:HAS_GENRE]-> Genre
// Unique persons: 1538, Genres: 21, Movies: 400

import { IMDB_GENRES } from "./imdb/genres";
import { IMDB_PEOPLE } from "./imdb/people";
import { IMDB_MOVIES } from "./imdb/movies";
import { IMDB_DIRECTED } from "./imdb/directed";
import { IMDB_CAST } from "./imdb/cast";
import { IMDB_MOVIE_GENRES } from "./imdb/movie-genres";

export const IMDB_SEED_QUERIES: string[] = [
  ...IMDB_GENRES,
  ...IMDB_PEOPLE,
  ...IMDB_MOVIES,
  ...IMDB_DIRECTED,
  ...IMDB_CAST,
  ...IMDB_MOVIE_GENRES,
];
