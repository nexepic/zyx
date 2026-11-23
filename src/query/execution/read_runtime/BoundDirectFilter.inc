struct DirectFilterTerm {
    size_t column;
    detail::ColumnPredicateOp operation;
    PropertyValue scalar{};
    const PropertyValue *borrowed = nullptr;
    std::optional<std::string_view> literal_text{};
    [[nodiscard]] const PropertyValue &expected() const { return borrowed ? *borrowed : scalar; }
    [[nodiscard]] std::optional<std::string_view> text() const {
        if (literal_text) return literal_text;
        const auto *value = std::get_if<std::string>(&expected().getVariant());
        return value ? std::optional<std::string_view>(*value) : std::nullopt;
    }
};

template<class Reference, class Bind>
bool collectDirectFilter(const logical::LogicalFilter &filter, ExecutionState &state,
    const Reference &directReference, const Bind &bind, utils::AdmissionPmrVector<DirectFilterTerm> &directTerms) {
    const auto collect = [&](const auto &self, const expressions::Expression *expression) -> bool {
        checkGuard(state);
        const auto *binary = dynamic_cast<const expressions::BinaryOpExpression *>(expression);
        if (!binary || !binary->getLeft() || !binary->getRight()) return false;
        using Op = expressions::BinaryOperatorType;
        if (binary->getOperator() == Op::BOP_AND) return self(self, binary->getLeft()) && self(self, binary->getRight());
        const auto *ref = directReference(binary->getLeft());
        const auto *literal = dynamic_cast<const expressions::LiteralExpression *>(binary->getRight());
        const auto *parameter = dynamic_cast<const expressions::ParameterExpression *>(binary->getRight());
        if (!ref || !ref->hasProperty() || (!literal && !parameter)) return false;
        detail::ColumnPredicateOp operation;
        switch (binary->getOperator()) {
            case Op::BOP_EQUAL: operation = detail::ColumnPredicateOp::CPO_EQUAL; break;
            case Op::BOP_NOT_EQUAL: operation = detail::ColumnPredicateOp::CPO_NOT_EQUAL; break;
            case Op::BOP_LESS: operation = detail::ColumnPredicateOp::CPO_LESS; break;
            case Op::BOP_LESS_EQUAL: operation = detail::ColumnPredicateOp::CPO_LESS_EQUAL; break;
            case Op::BOP_GREATER: operation = detail::ColumnPredicateOp::CPO_GREATER; break;
            case Op::BOP_GREATER_EQUAL: operation = detail::ColumnPredicateOp::CPO_GREATER_EQUAL; break;
            default: return false;
        }
        DirectFilterTerm term{bind(binary->getLeft()), operation};
        if (parameter) {
            // QueryContext owns immutable parameters for this execution. Borrow
            // the actual value, not a literal or a cached plan's previous value.
            if (!state.shared.context) return false;
            const auto &parameters = state.shared.context->parameters;
            const auto found = parameters.find(parameter->getParameterName());
            // Let the row evaluator retain its missing-parameter error timing.
            if (found == parameters.end()) return false;
            term.borrowed = &found->second;
        } else if (literal->isString()) {
            term.scalar = PropertyValue(std::string{});
            term.literal_text = literal->getStringValue();
        } else if (literal->isBoolean()) term.scalar = PropertyValue(literal->getBooleanValue());
        else if (literal->isInteger()) term.scalar = PropertyValue(literal->getIntegerValue());
        else if (literal->isDouble()) term.scalar = PropertyValue(literal->getDoubleValue());
        else if (literal->isPropertyValue()) term.borrowed = &literal->getPropertyValue();
        directTerms.push_back(std::move(term));
        return true;
    };
    return collect(collect, filter.getPredicate().get());
}

void bindPayloadSelection(storage::PropertyColumnBatchReadSinks &runs, SelectionVector &selected,
    ExecutionState &state, std::span<const DirectFilterTerm> directTerms, bool &guardFailed) {
    runs.needs_payload = [&, directTerms](size_t row, PropertyType type, uint64_t bits, std::string_view text) {
        // Guard exceptions, including a budget exception from a guard, must
        // propagate without private byte-pressure replay of this source.
        try { checkGuard(state); } catch (...) { guardFailed = true; throw; }
        const auto matches = [&](auto value) {
            return std::ranges::all_of(directTerms, [&](const auto &term) {
                return detail::columnPredicateMatchesBorrowed(value,
                    {term.column, term.operation, &term.expected()}, term.literal_text);
            });
        };
        bool keep = true;
        switch (type) {
            case PropertyType::NULL_TYPE: keep = false; break;
            case PropertyType::INTEGER: keep = matches(std::bit_cast<int64_t>(bits)); break;
            case PropertyType::DOUBLE: keep = matches(std::bit_cast<double>(bits)); break;
            case PropertyType::BOOLEAN: keep = matches(bits != 0); break;
            case PropertyType::STRING: keep = matches(text); break;
            default: break;
        }
        if (!keep) selected.reject(row);
        return keep;
    };
}
