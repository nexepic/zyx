/**
 * @file CancellationCheckpoint.hpp
 * @brief Storage-neutral cooperative cancellation boundary.
 *
 * Copyright (c) 2026 Nexepic
 * Licensed under the Apache License, Version 2.0.
 */

#pragma once

namespace graph::storage {

	// Storage algorithms call this at bounded units of work. Implementations may
	// throw their owner-specific timeout or cancellation exception.
	class CancellationCheckpoint {
	public:
		virtual ~CancellationCheckpoint() = default;

		virtual void check() const = 0;
	};

} // namespace graph::storage
