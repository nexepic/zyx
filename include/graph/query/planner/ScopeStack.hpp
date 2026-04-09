/**
 * @file ScopeStack.hpp
 * @brief Unified variable scope management for Cypher query compilation.
 *
 * Handles variable visibility for WITH projection barriers,
 * CALL { ... } isolated subqueries, and FOREACH loop scopes.
 *
 * Licensed under the Apache License, Version 2.0
 **/

#pragma once

#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

namespace graph::query::planner {

class ScopeStack {
public:
	struct Frame {
		std::unordered_set<std::string> variables;
		bool isolated = false; // true for CALL { ... } subquery frames
	};

	ScopeStack() {
		// Start with one global frame
		frames_.emplace_back();
	}

	void define(const std::string &var) {
		frames_.back().variables.insert(var);
	}

	[[nodiscard]] bool resolve(const std::string &var) const {
		// Walk frames from top to bottom
		for (auto it = frames_.rbegin(); it != frames_.rend(); ++it) {
			if (it->variables.count(var)) {
				return true;
			}
			// Isolated frames block lookup into outer scopes
			if (it->isolated) {
				return false;
			}
		}
		return false;
	}

	void pushFrame(bool isolated = false) {
		Frame f;
		f.isolated = isolated;
		frames_.push_back(std::move(f));
	}

	Frame popFrame() {
		if (frames_.size() <= 1) {
			throw std::logic_error("ScopeStack::popFrame: cannot pop the root frame");
		}
		Frame f = std::move(frames_.back());
		frames_.pop_back();
		return f;
	}

	void replaceFrame(std::unordered_set<std::string> vars) {
		frames_.back().variables = std::move(vars);
	}

	[[nodiscard]] std::unordered_set<std::string> currentVars() const {
		return frames_.back().variables;
	}

	[[nodiscard]] size_t depth() const { return frames_.size(); }

private:
	std::vector<Frame> frames_;
};

} // namespace graph::query::planner
