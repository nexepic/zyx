//----------------- ExpressionContext ------------------------------------------------------------------

CypherParser::ExpressionContext::ExpressionContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

CypherParser::OrExpressionContext* CypherParser::ExpressionContext::orExpression() {
  return getRuleContext<CypherParser::OrExpressionContext>(0);
}


size_t CypherParser::ExpressionContext::getRuleIndex() const {
  return CypherParser::RuleExpression;
}


std::any CypherParser::ExpressionContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitExpression(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::ExpressionContext* CypherParser::expression() {
  ExpressionContext *_localctx = _tracker.createInstance<ExpressionContext>(_ctx, getState());
  enterRule(_localctx, 116, CypherParser::RuleExpression);

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    enterOuterAlt(_localctx, 1);
    setState(848);
    orExpression();
   
  }
  catch (RecognitionException &e) {
    _errHandler->reportError(this, e);
    _localctx->exception = std::current_exception();
    _errHandler->recover(this, _localctx->exception);
  }

  return _localctx;
}

//----------------- OrExpressionContext ------------------------------------------------------------------

CypherParser::OrExpressionContext::OrExpressionContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

std::vector<CypherParser::XorExpressionContext *> CypherParser::OrExpressionContext::xorExpression() {
  return getRuleContexts<CypherParser::XorExpressionContext>();
}

CypherParser::XorExpressionContext* CypherParser::OrExpressionContext::xorExpression(size_t i) {
  return getRuleContext<CypherParser::XorExpressionContext>(i);
}

std::vector<tree::TerminalNode *> CypherParser::OrExpressionContext::K_OR() {
  return getTokens(CypherParser::K_OR);
}

tree::TerminalNode* CypherParser::OrExpressionContext::K_OR(size_t i) {
  return getToken(CypherParser::K_OR, i);
}


size_t CypherParser::OrExpressionContext::getRuleIndex() const {
  return CypherParser::RuleOrExpression;
}


std::any CypherParser::OrExpressionContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitOrExpression(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::OrExpressionContext* CypherParser::orExpression() {
  OrExpressionContext *_localctx = _tracker.createInstance<OrExpressionContext>(_ctx, getState());
  enterRule(_localctx, 118, CypherParser::RuleOrExpression);
  size_t _la = 0;

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    enterOuterAlt(_localctx, 1);
    setState(850);
    xorExpression();
    setState(855);
    _errHandler->sync(this);
    _la = _input->LA(1);
    while (_la == CypherParser::K_OR) {
      setState(851);
      match(CypherParser::K_OR);
      setState(852);
      xorExpression();
      setState(857);
      _errHandler->sync(this);
      _la = _input->LA(1);
    }
   
  }
  catch (RecognitionException &e) {
    _errHandler->reportError(this, e);
    _localctx->exception = std::current_exception();
    _errHandler->recover(this, _localctx->exception);
  }

  return _localctx;
}

//----------------- XorExpressionContext ------------------------------------------------------------------

CypherParser::XorExpressionContext::XorExpressionContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

std::vector<CypherParser::AndExpressionContext *> CypherParser::XorExpressionContext::andExpression() {
  return getRuleContexts<CypherParser::AndExpressionContext>();
}

CypherParser::AndExpressionContext* CypherParser::XorExpressionContext::andExpression(size_t i) {
  return getRuleContext<CypherParser::AndExpressionContext>(i);
}

std::vector<tree::TerminalNode *> CypherParser::XorExpressionContext::K_XOR() {
  return getTokens(CypherParser::K_XOR);
}

tree::TerminalNode* CypherParser::XorExpressionContext::K_XOR(size_t i) {
  return getToken(CypherParser::K_XOR, i);
}


size_t CypherParser::XorExpressionContext::getRuleIndex() const {
  return CypherParser::RuleXorExpression;
}


std::any CypherParser::XorExpressionContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitXorExpression(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::XorExpressionContext* CypherParser::xorExpression() {
  XorExpressionContext *_localctx = _tracker.createInstance<XorExpressionContext>(_ctx, getState());
  enterRule(_localctx, 120, CypherParser::RuleXorExpression);
  size_t _la = 0;

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    enterOuterAlt(_localctx, 1);
    setState(858);
    andExpression();
    setState(863);
    _errHandler->sync(this);
    _la = _input->LA(1);
    while (_la == CypherParser::K_XOR) {
      setState(859);
      match(CypherParser::K_XOR);
      setState(860);
      andExpression();
      setState(865);
      _errHandler->sync(this);
      _la = _input->LA(1);
    }
   
  }
  catch (RecognitionException &e) {
    _errHandler->reportError(this, e);
    _localctx->exception = std::current_exception();
    _errHandler->recover(this, _localctx->exception);
  }

  return _localctx;
}

//----------------- AndExpressionContext ------------------------------------------------------------------

CypherParser::AndExpressionContext::AndExpressionContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

std::vector<CypherParser::NotExpressionContext *> CypherParser::AndExpressionContext::notExpression() {
  return getRuleContexts<CypherParser::NotExpressionContext>();
}

CypherParser::NotExpressionContext* CypherParser::AndExpressionContext::notExpression(size_t i) {
  return getRuleContext<CypherParser::NotExpressionContext>(i);
}

std::vector<tree::TerminalNode *> CypherParser::AndExpressionContext::K_AND() {
  return getTokens(CypherParser::K_AND);
}

tree::TerminalNode* CypherParser::AndExpressionContext::K_AND(size_t i) {
  return getToken(CypherParser::K_AND, i);
}


size_t CypherParser::AndExpressionContext::getRuleIndex() const {
  return CypherParser::RuleAndExpression;
}


std::any CypherParser::AndExpressionContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitAndExpression(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::AndExpressionContext* CypherParser::andExpression() {
  AndExpressionContext *_localctx = _tracker.createInstance<AndExpressionContext>(_ctx, getState());
  enterRule(_localctx, 122, CypherParser::RuleAndExpression);
  size_t _la = 0;

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    enterOuterAlt(_localctx, 1);
    setState(866);
    notExpression();
    setState(871);
    _errHandler->sync(this);
    _la = _input->LA(1);
    while (_la == CypherParser::K_AND) {
      setState(867);
      match(CypherParser::K_AND);
      setState(868);
      notExpression();
      setState(873);
      _errHandler->sync(this);
      _la = _input->LA(1);
    }
   
  }
  catch (RecognitionException &e) {
    _errHandler->reportError(this, e);
    _localctx->exception = std::current_exception();
    _errHandler->recover(this, _localctx->exception);
  }

  return _localctx;
}

//----------------- NotExpressionContext ------------------------------------------------------------------

CypherParser::NotExpressionContext::NotExpressionContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

CypherParser::ComparisonExpressionContext* CypherParser::NotExpressionContext::comparisonExpression() {
  return getRuleContext<CypherParser::ComparisonExpressionContext>(0);
}

tree::TerminalNode* CypherParser::NotExpressionContext::K_NOT() {
  return getToken(CypherParser::K_NOT, 0);
}


size_t CypherParser::NotExpressionContext::getRuleIndex() const {
  return CypherParser::RuleNotExpression;
}


std::any CypherParser::NotExpressionContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitNotExpression(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::NotExpressionContext* CypherParser::notExpression() {
  NotExpressionContext *_localctx = _tracker.createInstance<NotExpressionContext>(_ctx, getState());
  enterRule(_localctx, 124, CypherParser::RuleNotExpression);

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    enterOuterAlt(_localctx, 1);
    setState(875);
    _errHandler->sync(this);

    switch (getInterpreter<atn::ParserATNSimulator>()->adaptivePredict(_input, 103, _ctx)) {
    case 1: {
      setState(874);
      match(CypherParser::K_NOT);
      break;
    }

    default:
      break;
    }
    setState(877);
    comparisonExpression();
   
  }
  catch (RecognitionException &e) {
    _errHandler->reportError(this, e);
    _localctx->exception = std::current_exception();
    _errHandler->recover(this, _localctx->exception);
  }

  return _localctx;
}

//----------------- ComparisonExpressionContext ------------------------------------------------------------------

CypherParser::ComparisonExpressionContext::ComparisonExpressionContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

std::vector<CypherParser::ArithmeticExpressionContext *> CypherParser::ComparisonExpressionContext::arithmeticExpression() {
  return getRuleContexts<CypherParser::ArithmeticExpressionContext>();
}

CypherParser::ArithmeticExpressionContext* CypherParser::ComparisonExpressionContext::arithmeticExpression(size_t i) {
  return getRuleContext<CypherParser::ArithmeticExpressionContext>(i);
}

tree::TerminalNode* CypherParser::ComparisonExpressionContext::K_BETWEEN() {
  return getToken(CypherParser::K_BETWEEN, 0);
}

tree::TerminalNode* CypherParser::ComparisonExpressionContext::K_AND() {
  return getToken(CypherParser::K_AND, 0);
}

tree::TerminalNode* CypherParser::ComparisonExpressionContext::K_IS() {
  return getToken(CypherParser::K_IS, 0);
}

tree::TerminalNode* CypherParser::ComparisonExpressionContext::K_NULL() {
  return getToken(CypherParser::K_NULL, 0);
}

std::vector<tree::TerminalNode *> CypherParser::ComparisonExpressionContext::EQ() {
  return getTokens(CypherParser::EQ);
}

tree::TerminalNode* CypherParser::ComparisonExpressionContext::EQ(size_t i) {
  return getToken(CypherParser::EQ, i);
}

std::vector<tree::TerminalNode *> CypherParser::ComparisonExpressionContext::NEQ() {
  return getTokens(CypherParser::NEQ);
}

tree::TerminalNode* CypherParser::ComparisonExpressionContext::NEQ(size_t i) {
  return getToken(CypherParser::NEQ, i);
}

std::vector<tree::TerminalNode *> CypherParser::ComparisonExpressionContext::LT() {
  return getTokens(CypherParser::LT);
}

tree::TerminalNode* CypherParser::ComparisonExpressionContext::LT(size_t i) {
  return getToken(CypherParser::LT, i);
}

std::vector<tree::TerminalNode *> CypherParser::ComparisonExpressionContext::GT() {
  return getTokens(CypherParser::GT);
}

tree::TerminalNode* CypherParser::ComparisonExpressionContext::GT(size_t i) {
  return getToken(CypherParser::GT, i);
}

std::vector<tree::TerminalNode *> CypherParser::ComparisonExpressionContext::LTE() {
  return getTokens(CypherParser::LTE);
}

tree::TerminalNode* CypherParser::ComparisonExpressionContext::LTE(size_t i) {
  return getToken(CypherParser::LTE, i);
}

std::vector<tree::TerminalNode *> CypherParser::ComparisonExpressionContext::GTE() {
  return getTokens(CypherParser::GTE);
}

tree::TerminalNode* CypherParser::ComparisonExpressionContext::GTE(size_t i) {
  return getToken(CypherParser::GTE, i);
}

std::vector<tree::TerminalNode *> CypherParser::ComparisonExpressionContext::REGEX_MATCH() {
  return getTokens(CypherParser::REGEX_MATCH);
}

tree::TerminalNode* CypherParser::ComparisonExpressionContext::REGEX_MATCH(size_t i) {
  return getToken(CypherParser::REGEX_MATCH, i);
}

std::vector<tree::TerminalNode *> CypherParser::ComparisonExpressionContext::K_IN() {
  return getTokens(CypherParser::K_IN);
}

tree::TerminalNode* CypherParser::ComparisonExpressionContext::K_IN(size_t i) {
  return getToken(CypherParser::K_IN, i);
}

std::vector<tree::TerminalNode *> CypherParser::ComparisonExpressionContext::K_STARTS() {
  return getTokens(CypherParser::K_STARTS);
}

tree::TerminalNode* CypherParser::ComparisonExpressionContext::K_STARTS(size_t i) {
  return getToken(CypherParser::K_STARTS, i);
}

std::vector<tree::TerminalNode *> CypherParser::ComparisonExpressionContext::K_WITH() {
  return getTokens(CypherParser::K_WITH);
}

tree::TerminalNode* CypherParser::ComparisonExpressionContext::K_WITH(size_t i) {
  return getToken(CypherParser::K_WITH, i);
}

std::vector<tree::TerminalNode *> CypherParser::ComparisonExpressionContext::K_ENDS() {
  return getTokens(CypherParser::K_ENDS);
}

tree::TerminalNode* CypherParser::ComparisonExpressionContext::K_ENDS(size_t i) {
  return getToken(CypherParser::K_ENDS, i);
}

std::vector<tree::TerminalNode *> CypherParser::ComparisonExpressionContext::K_CONTAINS() {
  return getTokens(CypherParser::K_CONTAINS);
}

tree::TerminalNode* CypherParser::ComparisonExpressionContext::K_CONTAINS(size_t i) {
  return getToken(CypherParser::K_CONTAINS, i);
}

tree::TerminalNode* CypherParser::ComparisonExpressionContext::K_NOT() {
  return getToken(CypherParser::K_NOT, 0);
}


size_t CypherParser::ComparisonExpressionContext::getRuleIndex() const {
  return CypherParser::RuleComparisonExpression;
}


std::any CypherParser::ComparisonExpressionContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitComparisonExpression(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::ComparisonExpressionContext* CypherParser::comparisonExpression() {
  ComparisonExpressionContext *_localctx = _tracker.createInstance<ComparisonExpressionContext>(_ctx, getState());
  enterRule(_localctx, 126, CypherParser::RuleComparisonExpression);
  size_t _la = 0;

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    setState(914);
    _errHandler->sync(this);
    switch (getInterpreter<atn::ParserATNSimulator>()->adaptivePredict(_input, 108, _ctx)) {
    case 1: {
      enterOuterAlt(_localctx, 1);
      setState(879);
      arithmeticExpression();
      setState(880);
      match(CypherParser::K_BETWEEN);
      setState(881);
      arithmeticExpression();
      setState(882);
      match(CypherParser::K_AND);
      setState(883);
      arithmeticExpression();
      break;
    }

    case 2: {
      enterOuterAlt(_localctx, 2);
      setState(885);
      arithmeticExpression();
      setState(904);
      _errHandler->sync(this);
      _la = _input->LA(1);
      while ((((_la & ~ 0x3fULL) == 0) &&
        ((1ULL << _la) & 18924699648) != 0) || ((((_la - 93) & ~ 0x3fULL) == 0) &&
        ((1ULL << (_la - 93)) & 127) != 0)) {
        setState(899);
        _errHandler->sync(this);
        switch (_input->LA(1)) {
          case CypherParser::EQ: {
            setState(886);
            match(CypherParser::EQ);
            break;
          }

          case CypherParser::NEQ: {
            setState(887);
            match(CypherParser::NEQ);
            break;
          }

          case CypherParser::LT: {
            setState(888);
            match(CypherParser::LT);
            break;
          }

          case CypherParser::GT: {
            setState(889);
            match(CypherParser::GT);
            break;
          }

          case CypherParser::LTE: {
            setState(890);
            match(CypherParser::LTE);
            break;
          }

          case CypherParser::GTE: {
            setState(891);
            match(CypherParser::GTE);
            break;
          }

          case CypherParser::REGEX_MATCH: {
            setState(892);
            match(CypherParser::REGEX_MATCH);
            break;
          }

          case CypherParser::K_IN: {
            setState(893);
            match(CypherParser::K_IN);
            break;
          }

          case CypherParser::K_STARTS: {
            setState(894);
            match(CypherParser::K_STARTS);
            setState(895);
            match(CypherParser::K_WITH);
            break;
          }

          case CypherParser::K_ENDS: {
            setState(896);
            match(CypherParser::K_ENDS);
            setState(897);
            match(CypherParser::K_WITH);
            break;
          }

          case CypherParser::K_CONTAINS: {
            setState(898);
            match(CypherParser::K_CONTAINS);
            break;
          }

        default:
          throw NoViableAltException(this);
        }
        setState(901);
        arithmeticExpression();
        setState(906);
        _errHandler->sync(this);
        _la = _input->LA(1);
      }
      setState(912);
      _errHandler->sync(this);

      _la = _input->LA(1);
      if (_la == CypherParser::K_IS) {
        setState(907);
        match(CypherParser::K_IS);
        setState(909);
        _errHandler->sync(this);

        _la = _input->LA(1);
        if (_la == CypherParser::K_NOT) {
          setState(908);
          match(CypherParser::K_NOT);
        }
        setState(911);
        match(CypherParser::K_NULL);
      }
      break;
    }

    default:
      break;
    }
   
  }
  catch (RecognitionException &e) {
    _errHandler->reportError(this, e);
    _localctx->exception = std::current_exception();
    _errHandler->recover(this, _localctx->exception);
  }

  return _localctx;
}

//----------------- ArithmeticExpressionContext ------------------------------------------------------------------

CypherParser::ArithmeticExpressionContext::ArithmeticExpressionContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

std::vector<CypherParser::PowerExpressionContext *> CypherParser::ArithmeticExpressionContext::powerExpression() {
  return getRuleContexts<CypherParser::PowerExpressionContext>();
}

CypherParser::PowerExpressionContext* CypherParser::ArithmeticExpressionContext::powerExpression(size_t i) {
  return getRuleContext<CypherParser::PowerExpressionContext>(i);
}

std::vector<tree::TerminalNode *> CypherParser::ArithmeticExpressionContext::PLUS() {
  return getTokens(CypherParser::PLUS);
}

tree::TerminalNode* CypherParser::ArithmeticExpressionContext::PLUS(size_t i) {
  return getToken(CypherParser::PLUS, i);
}

std::vector<tree::TerminalNode *> CypherParser::ArithmeticExpressionContext::MINUS() {
  return getTokens(CypherParser::MINUS);
}

tree::TerminalNode* CypherParser::ArithmeticExpressionContext::MINUS(size_t i) {
  return getToken(CypherParser::MINUS, i);
}

std::vector<tree::TerminalNode *> CypherParser::ArithmeticExpressionContext::MULTIPLY() {
  return getTokens(CypherParser::MULTIPLY);
}

tree::TerminalNode* CypherParser::ArithmeticExpressionContext::MULTIPLY(size_t i) {
  return getToken(CypherParser::MULTIPLY, i);
}

std::vector<tree::TerminalNode *> CypherParser::ArithmeticExpressionContext::DIVIDE() {
  return getTokens(CypherParser::DIVIDE);
}

tree::TerminalNode* CypherParser::ArithmeticExpressionContext::DIVIDE(size_t i) {
  return getToken(CypherParser::DIVIDE, i);
}

std::vector<tree::TerminalNode *> CypherParser::ArithmeticExpressionContext::MODULO() {
  return getTokens(CypherParser::MODULO);
}

tree::TerminalNode* CypherParser::ArithmeticExpressionContext::MODULO(size_t i) {
  return getToken(CypherParser::MODULO, i);
}


size_t CypherParser::ArithmeticExpressionContext::getRuleIndex() const {
  return CypherParser::RuleArithmeticExpression;
}


std::any CypherParser::ArithmeticExpressionContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitArithmeticExpression(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::ArithmeticExpressionContext* CypherParser::arithmeticExpression() {
  ArithmeticExpressionContext *_localctx = _tracker.createInstance<ArithmeticExpressionContext>(_ctx, getState());
  enterRule(_localctx, 128, CypherParser::RuleArithmeticExpression);
  size_t _la = 0;

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    enterOuterAlt(_localctx, 1);
    setState(916);
    powerExpression();
    setState(921);
    _errHandler->sync(this);
    _la = _input->LA(1);
    while (((((_la - 100) & ~ 0x3fULL) == 0) &&
      ((1ULL << (_la - 100)) & 31) != 0)) {
      setState(917);
      _la = _input->LA(1);
      if (!(((((_la - 100) & ~ 0x3fULL) == 0) &&
        ((1ULL << (_la - 100)) & 31) != 0))) {
      _errHandler->recoverInline(this);
      }
      else {
        _errHandler->reportMatch(this);
        consume();
      }
      setState(918);
      powerExpression();
      setState(923);
      _errHandler->sync(this);
      _la = _input->LA(1);
    }
   
  }
  catch (RecognitionException &e) {
    _errHandler->reportError(this, e);
    _localctx->exception = std::current_exception();
    _errHandler->recover(this, _localctx->exception);
  }

  return _localctx;
}

//----------------- PowerExpressionContext ------------------------------------------------------------------

CypherParser::PowerExpressionContext::PowerExpressionContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

std::vector<CypherParser::UnaryExpressionContext *> CypherParser::PowerExpressionContext::unaryExpression() {
  return getRuleContexts<CypherParser::UnaryExpressionContext>();
}

CypherParser::UnaryExpressionContext* CypherParser::PowerExpressionContext::unaryExpression(size_t i) {
  return getRuleContext<CypherParser::UnaryExpressionContext>(i);
}

std::vector<tree::TerminalNode *> CypherParser::PowerExpressionContext::POWER() {
  return getTokens(CypherParser::POWER);
}

tree::TerminalNode* CypherParser::PowerExpressionContext::POWER(size_t i) {
  return getToken(CypherParser::POWER, i);
}


size_t CypherParser::PowerExpressionContext::getRuleIndex() const {
  return CypherParser::RulePowerExpression;
}


std::any CypherParser::PowerExpressionContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitPowerExpression(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::PowerExpressionContext* CypherParser::powerExpression() {
  PowerExpressionContext *_localctx = _tracker.createInstance<PowerExpressionContext>(_ctx, getState());
  enterRule(_localctx, 130, CypherParser::RulePowerExpression);
  size_t _la = 0;

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    enterOuterAlt(_localctx, 1);
    setState(924);
    unaryExpression();
    setState(929);
    _errHandler->sync(this);
    _la = _input->LA(1);
    while (_la == CypherParser::POWER) {
      setState(925);
      match(CypherParser::POWER);
      setState(926);
      unaryExpression();
      setState(931);
      _errHandler->sync(this);
      _la = _input->LA(1);
    }
   
  }
  catch (RecognitionException &e) {
    _errHandler->reportError(this, e);
    _localctx->exception = std::current_exception();
    _errHandler->recover(this, _localctx->exception);
  }

  return _localctx;
}

//----------------- UnaryExpressionContext ------------------------------------------------------------------

CypherParser::UnaryExpressionContext::UnaryExpressionContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

CypherParser::AtomContext* CypherParser::UnaryExpressionContext::atom() {
  return getRuleContext<CypherParser::AtomContext>(0);
}

std::vector<CypherParser::AccessorContext *> CypherParser::UnaryExpressionContext::accessor() {
  return getRuleContexts<CypherParser::AccessorContext>();
}

CypherParser::AccessorContext* CypherParser::UnaryExpressionContext::accessor(size_t i) {
  return getRuleContext<CypherParser::AccessorContext>(i);
}

tree::TerminalNode* CypherParser::UnaryExpressionContext::PLUS() {
  return getToken(CypherParser::PLUS, 0);
}

tree::TerminalNode* CypherParser::UnaryExpressionContext::MINUS() {
  return getToken(CypherParser::MINUS, 0);
}


size_t CypherParser::UnaryExpressionContext::getRuleIndex() const {
  return CypherParser::RuleUnaryExpression;
}


std::any CypherParser::UnaryExpressionContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitUnaryExpression(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::UnaryExpressionContext* CypherParser::unaryExpression() {
  UnaryExpressionContext *_localctx = _tracker.createInstance<UnaryExpressionContext>(_ctx, getState());
  enterRule(_localctx, 132, CypherParser::RuleUnaryExpression);
  size_t _la = 0;

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    enterOuterAlt(_localctx, 1);
    setState(933);
    _errHandler->sync(this);

    _la = _input->LA(1);
    if (_la == CypherParser::PLUS

    || _la == CypherParser::MINUS) {
      setState(932);
      _la = _input->LA(1);
      if (!(_la == CypherParser::PLUS

      || _la == CypherParser::MINUS)) {
      _errHandler->recoverInline(this);
      }
      else {
        _errHandler->reportMatch(this);
        consume();
      }
    }
    setState(935);
    atom();
    setState(939);
    _errHandler->sync(this);
    _la = _input->LA(1);
    while (_la == CypherParser::LBRACK

    || _la == CypherParser::DOT) {
      setState(936);
      accessor();
      setState(941);
      _errHandler->sync(this);
      _la = _input->LA(1);
    }
   
  }
  catch (RecognitionException &e) {
    _errHandler->reportError(this, e);
    _localctx->exception = std::current_exception();
    _errHandler->recover(this, _localctx->exception);
  }

  return _localctx;
}

//----------------- AccessorContext ------------------------------------------------------------------

CypherParser::AccessorContext::AccessorContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

tree::TerminalNode* CypherParser::AccessorContext::DOT() {
  return getToken(CypherParser::DOT, 0);
}

CypherParser::PropertyKeyNameContext* CypherParser::AccessorContext::propertyKeyName() {
  return getRuleContext<CypherParser::PropertyKeyNameContext>(0);
}

tree::TerminalNode* CypherParser::AccessorContext::LBRACK() {
  return getToken(CypherParser::LBRACK, 0);
}

tree::TerminalNode* CypherParser::AccessorContext::RBRACK() {
  return getToken(CypherParser::RBRACK, 0);
}

std::vector<CypherParser::ExpressionContext *> CypherParser::AccessorContext::expression() {
  return getRuleContexts<CypherParser::ExpressionContext>();
}

CypherParser::ExpressionContext* CypherParser::AccessorContext::expression(size_t i) {
  return getRuleContext<CypherParser::ExpressionContext>(i);
}

tree::TerminalNode* CypherParser::AccessorContext::RANGE() {
  return getToken(CypherParser::RANGE, 0);
}


size_t CypherParser::AccessorContext::getRuleIndex() const {
  return CypherParser::RuleAccessor;
}


std::any CypherParser::AccessorContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitAccessor(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::AccessorContext* CypherParser::accessor() {
  AccessorContext *_localctx = _tracker.createInstance<AccessorContext>(_ctx, getState());
  enterRule(_localctx, 134, CypherParser::RuleAccessor);
  size_t _la = 0;

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    setState(955);
    _errHandler->sync(this);
    switch (_input->LA(1)) {
      case CypherParser::DOT: {
        enterOuterAlt(_localctx, 1);
        setState(942);
        match(CypherParser::DOT);
        setState(943);
        propertyKeyName();
        break;
      }

      case CypherParser::LBRACK: {
        enterOuterAlt(_localctx, 2);
        setState(944);
        match(CypherParser::LBRACK);
        setState(946);
        _errHandler->sync(this);

        _la = _input->LA(1);
        if ((((_la & ~ 0x3fULL) == 0) &&
          ((1ULL << _la) & -2) != 0) || ((((_la - 64) & ~ 0x3fULL) == 0) &&
          ((1ULL << (_la - 64)) & 2274410377494134783) != 0)) {
          setState(945);
          expression();
        }
        setState(952);
        _errHandler->sync(this);

        _la = _input->LA(1);
        if (_la == CypherParser::RANGE) {
          setState(948);
          match(CypherParser::RANGE);
          setState(950);
          _errHandler->sync(this);

          _la = _input->LA(1);
          if ((((_la & ~ 0x3fULL) == 0) &&
            ((1ULL << _la) & -2) != 0) || ((((_la - 64) & ~ 0x3fULL) == 0) &&
            ((1ULL << (_la - 64)) & 2274410377494134783) != 0)) {
            setState(949);
            expression();
          }
        }
        setState(954);
        match(CypherParser::RBRACK);
        break;
      }

    default:
      throw NoViableAltException(this);
    }
   
  }
  catch (RecognitionException &e) {
    _errHandler->reportError(this, e);
    _localctx->exception = std::current_exception();
    _errHandler->recover(this, _localctx->exception);
  }

  return _localctx;
}

//----------------- AtomContext ------------------------------------------------------------------

CypherParser::AtomContext::AtomContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

CypherParser::LiteralContext* CypherParser::AtomContext::literal() {
  return getRuleContext<CypherParser::LiteralContext>(0);
}

CypherParser::ParameterContext* CypherParser::AtomContext::parameter() {
  return getRuleContext<CypherParser::ParameterContext>(0);
}

tree::TerminalNode* CypherParser::AtomContext::K_COUNT() {
  return getToken(CypherParser::K_COUNT, 0);
}

tree::TerminalNode* CypherParser::AtomContext::LPAREN() {
  return getToken(CypherParser::LPAREN, 0);
}

tree::TerminalNode* CypherParser::AtomContext::MULTIPLY() {
  return getToken(CypherParser::MULTIPLY, 0);
}

tree::TerminalNode* CypherParser::AtomContext::RPAREN() {
  return getToken(CypherParser::RPAREN, 0);
}

CypherParser::QuantifierExpressionContext* CypherParser::AtomContext::quantifierExpression() {
  return getRuleContext<CypherParser::QuantifierExpressionContext>(0);
}

CypherParser::ReduceExpressionContext* CypherParser::AtomContext::reduceExpression() {
  return getRuleContext<CypherParser::ReduceExpressionContext>(0);
}

CypherParser::ExistsExpressionContext* CypherParser::AtomContext::existsExpression() {
  return getRuleContext<CypherParser::ExistsExpressionContext>(0);
}

CypherParser::ShortestPathExpressionContext* CypherParser::AtomContext::shortestPathExpression() {
  return getRuleContext<CypherParser::ShortestPathExpressionContext>(0);
}

CypherParser::MapProjectionContext* CypherParser::AtomContext::mapProjection() {
  return getRuleContext<CypherParser::MapProjectionContext>(0);
}

CypherParser::FunctionInvocationContext* CypherParser::AtomContext::functionInvocation() {
  return getRuleContext<CypherParser::FunctionInvocationContext>(0);
}

CypherParser::CaseExpressionContext* CypherParser::AtomContext::caseExpression() {
  return getRuleContext<CypherParser::CaseExpressionContext>(0);
}

CypherParser::VariableContext* CypherParser::AtomContext::variable() {
  return getRuleContext<CypherParser::VariableContext>(0);
}

CypherParser::ExpressionContext* CypherParser::AtomContext::expression() {
  return getRuleContext<CypherParser::ExpressionContext>(0);
}

CypherParser::ListLiteralContext* CypherParser::AtomContext::listLiteral() {
  return getRuleContext<CypherParser::ListLiteralContext>(0);
}

CypherParser::ListComprehensionContext* CypherParser::AtomContext::listComprehension() {
  return getRuleContext<CypherParser::ListComprehensionContext>(0);
}

CypherParser::PatternComprehensionContext* CypherParser::AtomContext::patternComprehension() {
  return getRuleContext<CypherParser::PatternComprehensionContext>(0);
}


size_t CypherParser::AtomContext::getRuleIndex() const {
  return CypherParser::RuleAtom;
}


std::any CypherParser::AtomContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitAtom(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::AtomContext* CypherParser::atom() {
  AtomContext *_localctx = _tracker.createInstance<AtomContext>(_ctx, getState());
  enterRule(_localctx, 136, CypherParser::RuleAtom);

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    setState(978);
    _errHandler->sync(this);
    switch (getInterpreter<atn::ParserATNSimulator>()->adaptivePredict(_input, 117, _ctx)) {
    case 1: {
      enterOuterAlt(_localctx, 1);
      setState(957);
      literal();
      break;
    }

    case 2: {
      enterOuterAlt(_localctx, 2);
      setState(958);
      parameter();
      break;
    }

    case 3: {
      enterOuterAlt(_localctx, 3);
      setState(959);
      match(CypherParser::K_COUNT);
      setState(960);
      match(CypherParser::LPAREN);
      setState(961);
      match(CypherParser::MULTIPLY);
      setState(962);
      match(CypherParser::RPAREN);
      break;
    }

    case 4: {
      enterOuterAlt(_localctx, 4);
      setState(963);
      quantifierExpression();
      break;
    }

    case 5: {
      enterOuterAlt(_localctx, 5);
      setState(964);
      reduceExpression();
      break;
    }

    case 6: {
      enterOuterAlt(_localctx, 6);
      setState(965);
      existsExpression();
      break;
    }

    case 7: {
      enterOuterAlt(_localctx, 7);
      setState(966);
      shortestPathExpression();
      break;
    }

    case 8: {
      enterOuterAlt(_localctx, 8);
      setState(967);
      mapProjection();
      break;
    }

    case 9: {
      enterOuterAlt(_localctx, 9);
      setState(968);
      functionInvocation();
      break;
    }

    case 10: {
      enterOuterAlt(_localctx, 10);
      setState(969);
      caseExpression();
      break;
    }

    case 11: {
      enterOuterAlt(_localctx, 11);
      setState(970);
      variable();
      break;
    }

    case 12: {
      enterOuterAlt(_localctx, 12);
      setState(971);
      match(CypherParser::LPAREN);
      setState(972);
      expression();
      setState(973);
      match(CypherParser::RPAREN);
      break;
    }

    case 13: {
      enterOuterAlt(_localctx, 13);
      setState(975);
      listLiteral();
      break;
    }

    case 14: {
      enterOuterAlt(_localctx, 14);
      setState(976);
      listComprehension();
      break;
    }

    case 15: {
      enterOuterAlt(_localctx, 15);
      setState(977);
      patternComprehension();
      break;
    }

    default:
      break;
    }
   
  }
  catch (RecognitionException &e) {
    _errHandler->reportError(this, e);
    _localctx->exception = std::current_exception();
    _errHandler->recover(this, _localctx->exception);
  }

  return _localctx;
}

//----------------- QuantifierExpressionContext ------------------------------------------------------------------

CypherParser::QuantifierExpressionContext::QuantifierExpressionContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

tree::TerminalNode* CypherParser::QuantifierExpressionContext::LPAREN() {
  return getToken(CypherParser::LPAREN, 0);
}

CypherParser::VariableContext* CypherParser::QuantifierExpressionContext::variable() {
  return getRuleContext<CypherParser::VariableContext>(0);
}

tree::TerminalNode* CypherParser::QuantifierExpressionContext::K_IN() {
  return getToken(CypherParser::K_IN, 0);
}

std::vector<CypherParser::ExpressionContext *> CypherParser::QuantifierExpressionContext::expression() {
  return getRuleContexts<CypherParser::ExpressionContext>();
}

CypherParser::ExpressionContext* CypherParser::QuantifierExpressionContext::expression(size_t i) {
  return getRuleContext<CypherParser::ExpressionContext>(i);
}

tree::TerminalNode* CypherParser::QuantifierExpressionContext::K_WHERE() {
  return getToken(CypherParser::K_WHERE, 0);
}

tree::TerminalNode* CypherParser::QuantifierExpressionContext::RPAREN() {
  return getToken(CypherParser::RPAREN, 0);
}

tree::TerminalNode* CypherParser::QuantifierExpressionContext::K_ALL() {
  return getToken(CypherParser::K_ALL, 0);
}

tree::TerminalNode* CypherParser::QuantifierExpressionContext::K_ANY() {
  return getToken(CypherParser::K_ANY, 0);
}

tree::TerminalNode* CypherParser::QuantifierExpressionContext::K_NONE() {
  return getToken(CypherParser::K_NONE, 0);
}

tree::TerminalNode* CypherParser::QuantifierExpressionContext::K_SINGLE() {
  return getToken(CypherParser::K_SINGLE, 0);
}


size_t CypherParser::QuantifierExpressionContext::getRuleIndex() const {
  return CypherParser::RuleQuantifierExpression;
}


std::any CypherParser::QuantifierExpressionContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitQuantifierExpression(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::QuantifierExpressionContext* CypherParser::quantifierExpression() {
  QuantifierExpressionContext *_localctx = _tracker.createInstance<QuantifierExpressionContext>(_ctx, getState());
  enterRule(_localctx, 138, CypherParser::RuleQuantifierExpression);
  size_t _la = 0;

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    enterOuterAlt(_localctx, 1);
    setState(980);
    _la = _input->LA(1);
    if (!((((_la & ~ 0x3fULL) == 0) &&
      ((1ULL << _la) & 4222124650659840) != 0))) {
    _errHandler->recoverInline(this);
    }
    else {
      _errHandler->reportMatch(this);
      consume();
    }
    setState(981);
    match(CypherParser::LPAREN);
    setState(982);
    variable();
    setState(983);
    match(CypherParser::K_IN);
    setState(984);
    expression();
    setState(985);
    match(CypherParser::K_WHERE);
    setState(986);
    expression();
    setState(987);
    match(CypherParser::RPAREN);
   
  }
  catch (RecognitionException &e) {
    _errHandler->reportError(this, e);
    _localctx->exception = std::current_exception();
    _errHandler->recover(this, _localctx->exception);
  }

  return _localctx;
}

//----------------- ExistsExpressionContext ------------------------------------------------------------------

CypherParser::ExistsExpressionContext::ExistsExpressionContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

tree::TerminalNode* CypherParser::ExistsExpressionContext::K_EXISTS() {
  return getToken(CypherParser::K_EXISTS, 0);
}

tree::TerminalNode* CypherParser::ExistsExpressionContext::LPAREN() {
  return getToken(CypherParser::LPAREN, 0);
}

CypherParser::PatternElementContext* CypherParser::ExistsExpressionContext::patternElement() {
  return getRuleContext<CypherParser::PatternElementContext>(0);
}

tree::TerminalNode* CypherParser::ExistsExpressionContext::RPAREN() {
  return getToken(CypherParser::RPAREN, 0);
}

tree::TerminalNode* CypherParser::ExistsExpressionContext::K_WHERE() {
  return getToken(CypherParser::K_WHERE, 0);
}

CypherParser::ExpressionContext* CypherParser::ExistsExpressionContext::expression() {
  return getRuleContext<CypherParser::ExpressionContext>(0);
}


size_t CypherParser::ExistsExpressionContext::getRuleIndex() const {
  return CypherParser::RuleExistsExpression;
}


std::any CypherParser::ExistsExpressionContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitExistsExpression(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::ExistsExpressionContext* CypherParser::existsExpression() {
  ExistsExpressionContext *_localctx = _tracker.createInstance<ExistsExpressionContext>(_ctx, getState());
  enterRule(_localctx, 140, CypherParser::RuleExistsExpression);
  size_t _la = 0;

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    enterOuterAlt(_localctx, 1);
    setState(989);
    match(CypherParser::K_EXISTS);
    setState(990);
    match(CypherParser::LPAREN);
    setState(991);
    patternElement();
    setState(994);
    _errHandler->sync(this);

    _la = _input->LA(1);
    if (_la == CypherParser::K_WHERE) {
      setState(992);
      match(CypherParser::K_WHERE);
      setState(993);
      expression();
    }
    setState(996);
    match(CypherParser::RPAREN);
   
  }
  catch (RecognitionException &e) {
    _errHandler->reportError(this, e);
    _localctx->exception = std::current_exception();
    _errHandler->recover(this, _localctx->exception);
  }

  return _localctx;
}

//----------------- CaseExpressionContext ------------------------------------------------------------------

CypherParser::CaseExpressionContext::CaseExpressionContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

tree::TerminalNode* CypherParser::CaseExpressionContext::K_CASE() {
  return getToken(CypherParser::K_CASE, 0);
}

tree::TerminalNode* CypherParser::CaseExpressionContext::K_END() {
  return getToken(CypherParser::K_END, 0);
}

std::vector<CypherParser::ExpressionContext *> CypherParser::CaseExpressionContext::expression() {
  return getRuleContexts<CypherParser::ExpressionContext>();
}

CypherParser::ExpressionContext* CypherParser::CaseExpressionContext::expression(size_t i) {
  return getRuleContext<CypherParser::ExpressionContext>(i);
}

std::vector<tree::TerminalNode *> CypherParser::CaseExpressionContext::K_WHEN() {
  return getTokens(CypherParser::K_WHEN);
}

tree::TerminalNode* CypherParser::CaseExpressionContext::K_WHEN(size_t i) {
  return getToken(CypherParser::K_WHEN, i);
}

std::vector<tree::TerminalNode *> CypherParser::CaseExpressionContext::K_THEN() {
  return getTokens(CypherParser::K_THEN);
}

tree::TerminalNode* CypherParser::CaseExpressionContext::K_THEN(size_t i) {
  return getToken(CypherParser::K_THEN, i);
}

tree::TerminalNode* CypherParser::CaseExpressionContext::K_ELSE() {
  return getToken(CypherParser::K_ELSE, 0);
}


size_t CypherParser::CaseExpressionContext::getRuleIndex() const {
  return CypherParser::RuleCaseExpression;
}


std::any CypherParser::CaseExpressionContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitCaseExpression(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::CaseExpressionContext* CypherParser::caseExpression() {
  CaseExpressionContext *_localctx = _tracker.createInstance<CaseExpressionContext>(_ctx, getState());
  enterRule(_localctx, 142, CypherParser::RuleCaseExpression);
  size_t _la = 0;

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    enterOuterAlt(_localctx, 1);
    setState(998);
    match(CypherParser::K_CASE);
    setState(1000);
    _errHandler->sync(this);

    switch (getInterpreter<atn::ParserATNSimulator>()->adaptivePredict(_input, 119, _ctx)) {
    case 1: {
      setState(999);
      expression();
      break;
    }

    default:
      break;
    }
    setState(1007); 
    _errHandler->sync(this);
    _la = _input->LA(1);
    do {
      setState(1002);
      match(CypherParser::K_WHEN);
      setState(1003);
      expression();
      setState(1004);
      match(CypherParser::K_THEN);
      setState(1005);
      expression();
      setState(1009); 
      _errHandler->sync(this);
      _la = _input->LA(1);
    } while (_la == CypherParser::K_WHEN);
    setState(1013);
    _errHandler->sync(this);

    _la = _input->LA(1);
    if (_la == CypherParser::K_ELSE) {
      setState(1011);
      match(CypherParser::K_ELSE);
      setState(1012);
      expression();
    }
    setState(1015);
    match(CypherParser::K_END);
   
  }
  catch (RecognitionException &e) {
    _errHandler->reportError(this, e);
    _localctx->exception = std::current_exception();
    _errHandler->recover(this, _localctx->exception);
  }

  return _localctx;
}

//----------------- PropertyExpressionContext ------------------------------------------------------------------

CypherParser::PropertyExpressionContext::PropertyExpressionContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

CypherParser::AtomContext* CypherParser::PropertyExpressionContext::atom() {
  return getRuleContext<CypherParser::AtomContext>(0);
}

std::vector<tree::TerminalNode *> CypherParser::PropertyExpressionContext::DOT() {
  return getTokens(CypherParser::DOT);
}

tree::TerminalNode* CypherParser::PropertyExpressionContext::DOT(size_t i) {
  return getToken(CypherParser::DOT, i);
}

std::vector<CypherParser::PropertyKeyNameContext *> CypherParser::PropertyExpressionContext::propertyKeyName() {
  return getRuleContexts<CypherParser::PropertyKeyNameContext>();
}

CypherParser::PropertyKeyNameContext* CypherParser::PropertyExpressionContext::propertyKeyName(size_t i) {
  return getRuleContext<CypherParser::PropertyKeyNameContext>(i);
}


size_t CypherParser::PropertyExpressionContext::getRuleIndex() const {
  return CypherParser::RulePropertyExpression;
}


std::any CypherParser::PropertyExpressionContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitPropertyExpression(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::PropertyExpressionContext* CypherParser::propertyExpression() {
  PropertyExpressionContext *_localctx = _tracker.createInstance<PropertyExpressionContext>(_ctx, getState());
  enterRule(_localctx, 144, CypherParser::RulePropertyExpression);
  size_t _la = 0;

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    enterOuterAlt(_localctx, 1);
    setState(1017);
    atom();
    setState(1020); 
    _errHandler->sync(this);
    _la = _input->LA(1);
    do {
      setState(1018);
      match(CypherParser::DOT);
      setState(1019);
      propertyKeyName();
      setState(1022); 
      _errHandler->sync(this);
      _la = _input->LA(1);
    } while (_la == CypherParser::DOT);
   
  }
  catch (RecognitionException &e) {
    _errHandler->reportError(this, e);
    _localctx->exception = std::current_exception();
    _errHandler->recover(this, _localctx->exception);
  }

  return _localctx;
}

//----------------- FunctionInvocationContext ------------------------------------------------------------------

CypherParser::FunctionInvocationContext::FunctionInvocationContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

CypherParser::FunctionNameContext* CypherParser::FunctionInvocationContext::functionName() {
  return getRuleContext<CypherParser::FunctionNameContext>(0);
}

tree::TerminalNode* CypherParser::FunctionInvocationContext::LPAREN() {
  return getToken(CypherParser::LPAREN, 0);
}

tree::TerminalNode* CypherParser::FunctionInvocationContext::RPAREN() {
  return getToken(CypherParser::RPAREN, 0);
}

tree::TerminalNode* CypherParser::FunctionInvocationContext::K_DISTINCT() {
  return getToken(CypherParser::K_DISTINCT, 0);
}

std::vector<CypherParser::ExpressionContext *> CypherParser::FunctionInvocationContext::expression() {
  return getRuleContexts<CypherParser::ExpressionContext>();
}

CypherParser::ExpressionContext* CypherParser::FunctionInvocationContext::expression(size_t i) {
  return getRuleContext<CypherParser::ExpressionContext>(i);
}

std::vector<tree::TerminalNode *> CypherParser::FunctionInvocationContext::COMMA() {
  return getTokens(CypherParser::COMMA);
}

tree::TerminalNode* CypherParser::FunctionInvocationContext::COMMA(size_t i) {
  return getToken(CypherParser::COMMA, i);
}


size_t CypherParser::FunctionInvocationContext::getRuleIndex() const {
  return CypherParser::RuleFunctionInvocation;
}


std::any CypherParser::FunctionInvocationContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitFunctionInvocation(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::FunctionInvocationContext* CypherParser::functionInvocation() {
  FunctionInvocationContext *_localctx = _tracker.createInstance<FunctionInvocationContext>(_ctx, getState());
  enterRule(_localctx, 146, CypherParser::RuleFunctionInvocation);
  size_t _la = 0;

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    enterOuterAlt(_localctx, 1);
    setState(1024);
    functionName();
    setState(1025);
    match(CypherParser::LPAREN);
    setState(1027);
    _errHandler->sync(this);

    switch (getInterpreter<atn::ParserATNSimulator>()->adaptivePredict(_input, 123, _ctx)) {
    case 1: {
      setState(1026);
      match(CypherParser::K_DISTINCT);
      break;
    }

    default:
      break;
    }
    setState(1037);
    _errHandler->sync(this);

    _la = _input->LA(1);
    if ((((_la & ~ 0x3fULL) == 0) &&
      ((1ULL << _la) & -2) != 0) || ((((_la - 64) & ~ 0x3fULL) == 0) &&
      ((1ULL << (_la - 64)) & 2274410377494134783) != 0)) {
      setState(1029);
      expression();
      setState(1034);
      _errHandler->sync(this);
      _la = _input->LA(1);
      while (_la == CypherParser::COMMA) {
        setState(1030);
        match(CypherParser::COMMA);
        setState(1031);
        expression();
        setState(1036);
        _errHandler->sync(this);
        _la = _input->LA(1);
      }
    }
    setState(1039);
    match(CypherParser::RPAREN);
   
  }
  catch (RecognitionException &e) {
    _errHandler->reportError(this, e);
    _localctx->exception = std::current_exception();
    _errHandler->recover(this, _localctx->exception);
  }

  return _localctx;
}

//----------------- ExplicitProcedureInvocationContext ------------------------------------------------------------------

CypherParser::ExplicitProcedureInvocationContext::ExplicitProcedureInvocationContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

CypherParser::ProcedureNameContext* CypherParser::ExplicitProcedureInvocationContext::procedureName() {
  return getRuleContext<CypherParser::ProcedureNameContext>(0);
}

tree::TerminalNode* CypherParser::ExplicitProcedureInvocationContext::LPAREN() {
  return getToken(CypherParser::LPAREN, 0);
}

tree::TerminalNode* CypherParser::ExplicitProcedureInvocationContext::RPAREN() {
  return getToken(CypherParser::RPAREN, 0);
}

std::vector<CypherParser::ExpressionContext *> CypherParser::ExplicitProcedureInvocationContext::expression() {
  return getRuleContexts<CypherParser::ExpressionContext>();
}

CypherParser::ExpressionContext* CypherParser::ExplicitProcedureInvocationContext::expression(size_t i) {
  return getRuleContext<CypherParser::ExpressionContext>(i);
}

std::vector<tree::TerminalNode *> CypherParser::ExplicitProcedureInvocationContext::COMMA() {
  return getTokens(CypherParser::COMMA);
}

tree::TerminalNode* CypherParser::ExplicitProcedureInvocationContext::COMMA(size_t i) {
  return getToken(CypherParser::COMMA, i);
}


size_t CypherParser::ExplicitProcedureInvocationContext::getRuleIndex() const {
  return CypherParser::RuleExplicitProcedureInvocation;
}


std::any CypherParser::ExplicitProcedureInvocationContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitExplicitProcedureInvocation(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::ExplicitProcedureInvocationContext* CypherParser::explicitProcedureInvocation() {
  ExplicitProcedureInvocationContext *_localctx = _tracker.createInstance<ExplicitProcedureInvocationContext>(_ctx, getState());
  enterRule(_localctx, 148, CypherParser::RuleExplicitProcedureInvocation);
  size_t _la = 0;

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    enterOuterAlt(_localctx, 1);
    setState(1041);
    procedureName();
    setState(1042);
    match(CypherParser::LPAREN);
    setState(1051);
    _errHandler->sync(this);

    _la = _input->LA(1);
    if ((((_la & ~ 0x3fULL) == 0) &&
      ((1ULL << _la) & -2) != 0) || ((((_la - 64) & ~ 0x3fULL) == 0) &&
      ((1ULL << (_la - 64)) & 2274410377494134783) != 0)) {
      setState(1043);
      expression();
      setState(1048);
      _errHandler->sync(this);
      _la = _input->LA(1);
      while (_la == CypherParser::COMMA) {
        setState(1044);
        match(CypherParser::COMMA);
        setState(1045);
        expression();
        setState(1050);
        _errHandler->sync(this);
        _la = _input->LA(1);
      }
    }
    setState(1053);
    match(CypherParser::RPAREN);
   
  }
  catch (RecognitionException &e) {
    _errHandler->reportError(this, e);
    _localctx->exception = std::current_exception();
    _errHandler->recover(this, _localctx->exception);
  }

  return _localctx;
}

//----------------- ImplicitProcedureInvocationContext ------------------------------------------------------------------

CypherParser::ImplicitProcedureInvocationContext::ImplicitProcedureInvocationContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

CypherParser::ProcedureNameContext* CypherParser::ImplicitProcedureInvocationContext::procedureName() {
  return getRuleContext<CypherParser::ProcedureNameContext>(0);
}


size_t CypherParser::ImplicitProcedureInvocationContext::getRuleIndex() const {
  return CypherParser::RuleImplicitProcedureInvocation;
}


std::any CypherParser::ImplicitProcedureInvocationContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitImplicitProcedureInvocation(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::ImplicitProcedureInvocationContext* CypherParser::implicitProcedureInvocation() {
  ImplicitProcedureInvocationContext *_localctx = _tracker.createInstance<ImplicitProcedureInvocationContext>(_ctx, getState());
  enterRule(_localctx, 150, CypherParser::RuleImplicitProcedureInvocation);

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    enterOuterAlt(_localctx, 1);
    setState(1055);
    procedureName();
   
  }
  catch (RecognitionException &e) {
    _errHandler->reportError(this, e);
    _localctx->exception = std::current_exception();
    _errHandler->recover(this, _localctx->exception);
  }

  return _localctx;
}

