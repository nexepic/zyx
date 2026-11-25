#pragma once
#include <array>
#include <cstddef>
#include <memory>

namespace zyx {
	class Database;
	class PublishedSnapshot final {
		using Root = std::array<std::byte, 32>;
		std::shared_ptr<const Root> root_;
		explicit PublishedSnapshot(Root root) : root_(std::make_shared<const Root>(root)) {}
		friend class Database;
	public:
		PublishedSnapshot(const PublishedSnapshot &) = default;
		PublishedSnapshot &operator=(const PublishedSnapshot &) = default;
		const Root &root() const noexcept { return *root_; }
	};
}
