#include "DisassemblyView.hpp"

#include "medusa/cell_action.hpp"

#include "Proxy.hpp"

DisassemblyView::DisassemblyView(QWidget * parent, medusa::Medusa * core)
  : QAbstractScrollArea(parent)
  , medusa::FullDisassemblyView(
    *core,
    new DisassemblyPrinter(*core),
    medusa::Printer::ShowAddress | medusa::Printer::AddSpaceBeforeXref,
    0, 0,
    (*core->GetDocument().Begin())->GetBaseAddress())
  , _needRepaint(true)
  , _core(core)
  , _xOffset(0),            _yOffset(10)
  , _wChar(0),              _hChar(0)
  , _xCursor(0),            _cursorAddress((*core->GetDocument().Begin())->GetBaseAddress())
  , _begSelection(0),       _endSelection(0)
  , _begSelectionOffset(0), _endSelectionOffset(0)
  , _addrLen(static_cast<int>((*core->GetDocument().Begin())->GetBaseAddress().ToString().length() + 1))
  , _lineNo(core->GetDocument().GetNumberOfAddress()), _lineLen(0x100)
  , _cursorTimer(),         _cursorBlink(false)
  , _visibleLines()
  , _curAddr()
  , _cache()
{
  setFont(); // this method initializes both _wChar and _hChar
  setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
  setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  connect(&_cursorTimer, SIGNAL(timeout()), this, SLOT(updateCursor()));
  _cursorTimer.setInterval(400);
  setContextMenuPolicy(Qt::CustomContextMenu);
  connect(this, SIGNAL(customContextMenuRequested(QPoint const &)), this, SLOT(showContextMenu(QPoint const &)));
  connect(horizontalScrollBar(), SIGNAL(valueChanged(int)), this, SLOT(horizontalScrollBarChanged(int)));

  connect(this, SIGNAL(DisassemblyViewAdded(medusa::Address const&)), this->parent(), SLOT(addDisassemblyView(medusa::Address const&)));
  connect(this, SIGNAL(SemanticViewAdded(medusa::Address const&)), this->parent(), SLOT(addSemanticView(medusa::Address const&)));
  connect(this, SIGNAL(ControlFlowGraphViewAdded(medusa::Address const&)), this->parent(), SLOT(addControlFlowGraphView(medusa::Address const&)));
  connect(this, SIGNAL(viewportUpdated()), this->viewport(), SLOT(update()));

  int w, h;
  QRect rect = viewport()->rect();
  w = rect.width() / _wChar + 1;
  h = rect.height() / _hChar + 1;
  Resize(w, h);
  viewUpdated();
}

DisassemblyView::~DisassemblyView(void)
{
}

void DisassemblyView::OnDocumentUpdated(void)
{
  emit viewUpdated();
}

bool DisassemblyView::goTo(medusa::Address const& address)
{
  if (_core->GetDocument().IsPresent(address) == false)
    return false;

  m_Mutex.lock();
  _cursorAddress = address;
  _Prepare(address);
  m_Mutex.unlock();
   viewUpdated();

  return true;
}

void DisassemblyView::setFont(void)
{
  QString fontInfo = Settings::instance().value(MEDUSA_FONT_TEXT, MEDUSA_FONT_TEXT_DEFAULT).toString();
  QFont font;
  font.fromString(fontInfo);
  QAbstractScrollArea::setFont(font);

  _wChar = fontMetrics().width('M');
  _hChar = fontMetrics().height();

  updateScrollbars();
  viewUpdated();
}

void DisassemblyView::viewUpdated(void)
{
  _lineNo = static_cast<int>(_core->GetDocument().GetNumberOfAddress());

  Refresh();

  emit viewportUpdated();
  _needRepaint = true;
}

void DisassemblyView::horizontalScrollBarChanged(int n)
{
  Move(-1, n);
  emit viewUpdated();
}

void DisassemblyView::listingUpdated(void)
{
  _lineNo = static_cast<int>(_core->GetDocument().GetNumberOfAddress());

  // OPTIMIZEME: this part of code is too time consumming
  // we should find a way to only update when it's necessary
  Refresh();

  viewport()->update();
  _needRepaint = true;
}

void DisassemblyView::updateCursor(void)
{
  _cursorBlink = _cursorBlink ? false : true;
  viewport()->update();
}

void DisassemblyView::showContextMenu(QPoint const & pos)
{
  QMenu menu;
  QPoint globalPos = viewport()->mapToGlobal(pos);

  medusa::Address::List selectedAddresses;
  getSelectedAddresses(selectedAddresses);

  if (selectedAddresses.size() == 0)
    selectedAddresses.push_back(_curAddr);

  medusa::CellAction::PtrList actions;
  medusa::CellAction::GetCellActionBuilders(actions);
  QHash<QAction*, medusa::CellAction*> actToCellAct;

  for (auto curAddress = std::begin(selectedAddresses); curAddress != std::end(selectedAddresses); ++curAddress)
  {
    medusa::Address selectedAddress = *curAddress;

    auto cell = _core->GetCell(selectedAddress);
    if (cell == nullptr) continue;

    if (actions.size() == 0) return;

    for (auto act = std::begin(actions); act != std::end(actions); ++act)
    {
      if ((*act)->IsCompatible(*cell) == false)
        continue;

      if (actToCellAct.values().contains(*act))
        continue;

      auto curAct = new QAction(QString::fromStdString((*act)->GetName()), this);
      curAct->setStatusTip(QString::fromStdString((*act)->GetDescription()));
      menu.addAction(curAct);
      actToCellAct[curAct] = *act;
    }
  }

  menu.addSeparator();

  AddDisassemblyViewAction      addDisasmViewAct;
  AddSemanticViewAction         addSemanticViewAct;
  AddControlFlowGraphViewAction addCfgViewAct;

  medusa::CellAction* qtAction[] = { &addDisasmViewAct, &addSemanticViewAct, &addCfgViewAct };
  std::for_each(std::begin(qtAction), std::end(qtAction), [&menu, &actToCellAct, this](medusa::CellAction* cellAct)
  {
    auto curAct = new QAction(QString::fromStdString(cellAct->GetName()), this);
    curAct->setStatusTip(QString::fromStdString(cellAct->GetDescription()));
    menu.addAction(curAct);
    actToCellAct[curAct] = cellAct;
  });

  QAction * selectedItem = menu.exec(globalPos);
  auto curAct = actToCellAct[selectedItem];

  if (curAct == nullptr)
    goto end;

  curAct->Do(*_core, selectedAddresses);

end:
    for (auto act = std::begin(actions); act != std::end(actions); ++act)
      delete *act;
}

void DisassemblyView::paintBackground(QPainter& p)
{
  // Draw background
  QColor addrColor = QColor(Settings::instance().value(MEDUSA_COLOR_ADDRESS_BACKGROUND, MEDUSA_COLOR_ADDRESS_BACKGROUND_DEFAULT).toString());
  QColor codeColor = QColor(Settings::instance().value(MEDUSA_COLOR_VIEW_BACKGROUND, MEDUSA_COLOR_VIEW_BACKGROUND_DEFAULT).toString());
  QColor cursColor = QColor(Qt::black);

  QRect addrRect = viewport()->rect();
  QRect codeRect = viewport()->rect();

  addrRect.setWidth((_addrLen - horizontalScrollBar()->value()) * _wChar);
  codeRect.setX((_addrLen - horizontalScrollBar()->value()) * _wChar);

  p.fillRect(addrRect, addrColor);
  p.fillRect(codeRect, codeColor);

  //p.fillRect(cursRect, cursColor);
}

void DisassemblyView::paintSelection(QPainter& p)
{
  return; // FIXME
  QColor slctColor = QColor(Settings::instance().value(MEDUSA_COLOR_INSTRUCTION_SELECTION, MEDUSA_COLOR_INSTRUCTION_SELECTION_DEFAULT).toString());

  _visibleLines.clear();
  _visibleLines.reserve(horizontalScrollBar()->maximum());

  int begSelect    = _begSelection;
  int endSelect    = _endSelection;
  int begSelectOff = _begSelectionOffset;
  int endSelectOff = _endSelectionOffset;
  int deltaSelect  = _endSelection - _begSelection;

  // If the user select from the bottom to the top, we have to swap up and down
  if (deltaSelect < 0)
  {
    deltaSelect  = -deltaSelect;
    begSelect    = _endSelection;
    endSelect    = _begSelection;
    begSelectOff = _endSelectionOffset;
    endSelectOff = _begSelectionOffset;
  }

  if (deltaSelect) deltaSelect++;

  if (deltaSelect == 0)
  {
    int deltaOffset = endSelectOff - begSelectOff;
    if (deltaOffset < 0)
    {
      deltaOffset = -deltaOffset;
      begSelectOff = _endSelectionOffset;
      endSelectOff = _begSelectionOffset;
    }
    int x = (begSelectOff - horizontalScrollBar()->value()) * _wChar;
    int y = (begSelect - verticalScrollBar()->value()) * _hChar;
    int w = deltaOffset * _wChar;
    int h = _hChar;
    QRect slctRect(x, y, w, h);
    p.fillRect(slctRect, slctColor);
  }

  // Draw selection background
  // This part is pretty tricky:
  // To draw the selection we use the lazy method by three passes.
  /*
     +-----------------+
     |     ############+ Part�
     |#################+ Part�
     |#################+ Part�
     |####             | Part�
     +-----------------+
  */
  else if (deltaSelect > 0)
  {
    // Part�
    int x = (begSelectOff - horizontalScrollBar()->value()) * _wChar;
    int y = (begSelect - verticalScrollBar()->value()) * _hChar;
    int w = (viewport()->width() - _addrLen) * _wChar;
    int h = _hChar;
    QRect slctRect(x, y, w, h);
    p.fillRect(slctRect, slctColor);

    // Part�
    if (deltaSelect > 2)
    {
      //auto limit = verticalScrollBar()->value() + viewport()->height();
      //if (limit && deltaSelect > limit)
      //  deltaSelect = limit;

      x = (_addrLen - horizontalScrollBar()->value()) * _wChar;
      y = slctRect.bottom();
      w = (viewport()->width() - _addrLen) * _wChar;
      h = (deltaSelect - 2) * _hChar;
      slctRect.setRect(x, y, w, h);
      p.fillRect(slctRect, slctColor);
    }

    // Part�
    x = (_addrLen - horizontalScrollBar()->value()) * _wChar;
    y = slctRect.bottom();
    w = (endSelectOff - _addrLen) * _wChar;
    h = _hChar;
    slctRect.setRect(x, y, w, h);
    p.fillRect(slctRect, slctColor);
  }
}

void DisassemblyView::paintText(QPainter& p)
{
  QFontMetrics fm = viewport()->fontMetrics();

  // TODO: find another way to use this method
  static_cast<DisassemblyPrinter*>(m_pPrinter)->SetPainter(&p);
  Print();
  medusa::u32 width, height;
  GetDimension(width, height);
  horizontalScrollBar()->setMaximum(static_cast<int>(width + _addrLen));
  static_cast<DisassemblyPrinter*>(m_pPrinter)->SetPainter(nullptr);
  return;
}

void DisassemblyView::paintCursor(QPainter& p)
{
  QColor codeColor = QColor(Settings::instance().value(MEDUSA_COLOR_VIEW_BACKGROUND, MEDUSA_COLOR_VIEW_BACKGROUND_DEFAULT).toString());

  // Draw cursor
  if (_cursorBlink)
  {
    QColor cursorColor = ~codeColor.value();

    int vertOff = 0;
    {
      boost::lock_guard<MutexType> Lock(m_Mutex);
      auto itAddr = std::find(std::begin(m_VisiblesAddresses), std::end(m_VisiblesAddresses), _cursorAddress);
      vertOff = static_cast<int>(std::distance(std::begin(m_VisiblesAddresses), itAddr));
    }

    QRect cursorRect((_xCursor - horizontalScrollBar()->value()) * _wChar, vertOff * _hChar, 2, _hChar);
    p.fillRect(cursorRect, cursorColor);
  }
}

void DisassemblyView::paintEvent(QPaintEvent * evt)
{
  if (_needRepaint == true)
  {
    _cache = QPixmap(viewport()->size());
    QPainter cachedPainter(&_cache);
    paintBackground(cachedPainter);
    paintSelection(cachedPainter);
    cachedPainter.setRenderHint(QPainter::TextAntialiasing);
    cachedPainter.setFont(font());
    paintText(cachedPainter);
    _needRepaint = false;
  }

  QPainter p(viewport());
  p.drawPixmap(0, 0, _cache);
  paintCursor(p);

  updateScrollbars();
}

void DisassemblyView::mouseMoveEvent(QMouseEvent * evt)
{
  medusa::Address addr;

  if (!convertMouseToAddress(evt, addr)) return;

  setCursorPosition(evt);

  if (evt->buttons() & Qt::LeftButton)
  {
    int xCursor = evt->x() / _wChar + horizontalScrollBar()->value();
    int yCursor = evt->y() / _hChar + verticalScrollBar()->value();

    if (xCursor < _addrLen)
      xCursor = _addrLen;
    _endSelection       = yCursor;
    _endSelectionOffset = xCursor;

    _needRepaint = true; // TODO: selectionChanged
  }
}

void DisassemblyView::mousePressEvent(QMouseEvent * evt)
{
  medusa::Address addr;
  if (convertMouseToAddress(evt, addr))
    _curAddr = addr;

  setCursorPosition(evt);

  if (evt->buttons() & Qt::LeftButton)
  {
    int xCursor = evt->x() / _wChar + horizontalScrollBar()->value();
    int yCursor = evt->y() / _hChar + verticalScrollBar()->value();

    if (xCursor < _addrLen)
      xCursor = _addrLen;
    _begSelection       = yCursor;
    _begSelectionOffset = xCursor;
    _endSelection       = yCursor;
    _endSelectionOffset = xCursor;

    _needRepaint = true; // TODO: selectionChanged
  }
}

void DisassemblyView::mouseDoubleClickEvent(QMouseEvent * evt)
{
  auto const& doc = _core->GetDocument();
  medusa::Address srcAddr, dstAddr;

  if (!convertMouseToAddress(evt, srcAddr)) return;

  auto insn = std::dynamic_pointer_cast<medusa::Instruction const>(doc.GetCell(srcAddr));
  if (insn == nullptr)
    return;

  auto memArea = doc.GetMemoryArea(srcAddr);

  for (medusa::u8 op = 0; op < 4; ++op)
  {
    if ( memArea != nullptr
      && insn->GetOperandReference(doc, op, srcAddr, dstAddr))
      if (goTo(dstAddr))
        return;
  }
}

void DisassemblyView::keyPressEvent(QKeyEvent * evt)
{
  // Move cursor
  if (evt->matches(QKeySequence::MoveToNextChar))
  { moveCursorPosition(+1, 0); resetSelection(); }
  if (evt->matches(QKeySequence::MoveToPreviousChar))
  { moveCursorPosition(-1, 0); resetSelection(); }

  if (evt->matches(QKeySequence::MoveToStartOfLine))
  { setCursorPosition(_addrLen, -1); resetSelection(); }
  if (evt->matches(QKeySequence::MoveToEndOfLine))
  {
    // FIXME
    //do
    //{
    //  int line = _yCursor - horizontalScrollBar()->value();
    //  if (line >= static_cast<int>(_visibleLines.size())) break;
    //  QString curLine = _visibleLines.at(static_cast<std::vector<QString>::size_type>(line));

    //  setCursorPosition(_addrLen + 1 + curLine.length(), -1);
    //} while (0);
    //resetSelection();
  }

  if (evt->matches(QKeySequence::MoveToNextLine))
  { moveCursorPosition(0, +1); resetSelection(); }
  if (evt->matches(QKeySequence::MoveToPreviousLine))
  { moveCursorPosition(0, -1); resetSelection(); }

  if (evt->matches(QKeySequence::MoveToNextPage))
  { moveCursorPosition(0, +(viewport()->rect().height() / _hChar)); resetSelection(); }
  if (evt->matches(QKeySequence::MoveToPreviousPage))
  { moveCursorPosition(0, -(viewport()->rect().height() / _hChar)); resetSelection(); }

  if (evt->matches(QKeySequence::MoveToStartOfDocument))
  { setCursorPosition(_addrLen, 0); resetSelection(); }
  if (evt->matches(QKeySequence::MoveToEndOfDocument))
  { setCursorPosition(_addrLen, horizontalScrollBar()->maximum() - 1); resetSelection(); }

  if (evt->matches(QKeySequence::MoveToNextWord))
  {
    // FIXME
    //do
    //{
    //  int line = _yCursor - horizontalScrollBar()->value();
    //  if (line >= static_cast<int>(_visibleLines.size())) break;

    //  QString curLine = _visibleLines.at(static_cast<std::vector<QString>::size_type>(line));

    //  int newPosition = curLine.indexOf(" ", _xCursor - _addrLen - 1);

    //  if      (newPosition == -1) newPosition = curLine.length();
    //  else if (newPosition < curLine.length() && newPosition == _xCursor - _addrLen - 1)
    //  {
    //    newPosition = curLine.indexOf(" ", _xCursor - _addrLen);
    //    if (newPosition == -1) newPosition = curLine.length();
    //  }

    //  setCursorPosition(newPosition + _addrLen + 1, -1);
    //} while (0);
    //resetSelection();
  }

  if (evt->matches(QKeySequence::MoveToPreviousWord))
  {
    // FIXME
    //do
    //{
    //  int line = _yCursor - horizontalScrollBar()->value();
    //  if (line >= static_cast<int>(_visibleLines.size())) break;

    //  QString curLine = _visibleLines.at(static_cast<std::vector<QString>::size_type>(line));

    //  if (_xCursor - _addrLen - 2 < 0) break;

    //  int newPosition = curLine.lastIndexOf(" ", _xCursor - _addrLen - 2);

    //  if (newPosition == 0) break;

    //  else if (newPosition == -1) newPosition = 0;

    //  setCursorPosition(newPosition + _addrLen + 1, -2);
    //} while (0);
    //resetSelection();
  }

  // Move selection
  if (evt->matches(QKeySequence::SelectNextChar))
    moveSelection(+1, 0);
  if (evt->matches(QKeySequence::SelectPreviousChar))
    moveSelection(-1, 0);

  if (evt->matches(QKeySequence::SelectStartOfLine))
    setSelection(_addrLen, -1);
  if (evt->matches(QKeySequence::SelectEndOfLine))
  {
    // FIXME
    //do
    //{
    //  int line = _yCursor - horizontalScrollBar()->value();
    //  if (line >= static_cast<int>(_visibleLines.size())) break;
    //  QString curLine = _visibleLines.at(static_cast<std::vector<QString>::size_type>(line));

    //  setSelection(_addrLen + 1 + curLine.length(), -1);
    //} while (0);
  }

  if (evt->matches(QKeySequence::SelectNextLine))
    moveSelection(0, +1);
  if (evt->matches(QKeySequence::SelectPreviousLine))
    moveSelection(0, -1);

  if (evt->matches(QKeySequence::SelectNextPage))
    moveSelection(0, +viewport()->rect().height());
  if (evt->matches(QKeySequence::SelectPreviousPage))
    moveSelection(0, -viewport()->rect().height());

  if (evt->matches(QKeySequence::SelectStartOfDocument))
    setSelection(-1, 0);
  if (evt->matches(QKeySequence::SelectEndOfDocument))
    setSelection(-1, horizontalScrollBar()->maximum());

  if (evt->matches(QKeySequence::SelectNextWord))
  {
    // FIXME
    //do
    //{
    //  int line = _yCursor - horizontalScrollBar()->value();
    //  if (line >= static_cast<int>(_visibleLines.size())) break;

    //  QString curLine = _visibleLines.at(static_cast<std::vector<QString>::size_type>(line));

    //  int newPosition = curLine.indexOf(" ", _xCursor - _addrLen - 1);

    //  if      (newPosition == -1) newPosition = curLine.length();
    //  else if (newPosition < curLine.length() && newPosition == _xCursor - _addrLen - 1)
    //  {
    //    newPosition = curLine.indexOf(" ", _xCursor - _addrLen);
    //    if (newPosition == -1) newPosition = curLine.length();
    //  }

    //  setSelection(newPosition + _addrLen + 1, -1);
    //} while (0);
  }

  if (evt->matches(QKeySequence::SelectPreviousWord))
  {
    // FIXME
    //do
    //{
    //  int line = _yCursor - horizontalScrollBar()->value();
    //  if (line >= static_cast<int>(_visibleLines.size())) break;

    //  QString curLine = _visibleLines.at(static_cast<std::vector<QString>::size_type>(line));

    //  if (_xCursor - _addrLen - 2 < 0) break;

    //  int newPosition = curLine.lastIndexOf(" ", _xCursor - _addrLen - 2);

    //  if (newPosition == 0) break;

    //  else if (newPosition == -1) newPosition = 0;

    //  setSelection(newPosition + _addrLen + 1, -2);
    //} while (0);
  }

  if (evt->matches(QKeySequence::SelectAll))
  {
    _begSelection       = 0;
    _endSelection       = _lineNo;
    _begSelectionOffset = _addrLen;
    _endSelectionOffset = -1; // TODO fix that
    //moveCursorPosition(_endSelectionOffset, _endSelection);
  }

  // Copy
  if (evt->matches(QKeySequence::Copy))
  {
    //do
    //{
    //  QClipboard * clipboard = QApplication::clipboard();

    //  if (_db == nullptr) break;

    //  if (_begSelection == _endSelection && _begSelectionOffset == _endSelectionOffset) break;

    //  int selectLineNr = _endSelection - _begSelection;
    //  int begLines     = _begSelection;
    //  int leftOffLine  = _begSelectionOffset - _addrLen - 1;
    //  int rightOffLine = _endSelectionOffset - _addrLen;

    //  if (selectLineNr == 0 && leftOffLine > rightOffLine)
    //  {
    //    leftOffLine    = _endSelectionOffset - _addrLen - 1;
    //    rightOffLine   = _begSelectionOffset - _addrLen;
    //  }

    //  else if (selectLineNr < 0)
    //  {
    //    selectLineNr   = -selectLineNr;
    //    begLines       = _endSelection;
    //    leftOffLine    = _endSelectionOffset - _addrLen - 1;
    //    rightOffLine   = _begSelectionOffset - _addrLen;
    //  }

    //  selectLineNr++;

    //  typedef medusa::View::LineInformation LineInformation;
    //  LineInformation lineInfo;

    //  QString clipboardBuf = "";
    //  for (int selectLine = 0; selectLine < selectLineNr; ++selectLine)
    //  {
    //    if (!_db->GetView().GetLineInformation(begLines + selectLine, lineInfo)) break;
    //    switch (lineInfo.GetType())
    //    {
    //    case LineInformation::EmptyLineType: clipboardBuf += "\n"; break;;

    //    case LineInformation::CellLineType:
    //      {
    //        medusa::Cell const * cell = _db->RetrieveCell(lineInfo.GetAddress());
    //        if (cell == nullptr) break;
    //        clipboardBuf += QString::fromStdString(cell->ToString());
    //        if (!cell->GetComment().empty())
    //          clipboardBuf += QString(" ; %1").arg(QString::fromStdString(cell->GetComment()));
    //        clipboardBuf += "\n";
    //        break;
    //      }

    //    case LineInformation::MultiCellLineType:
    //      {
    //        medusa::MultiCell const * multicell = _db->RetrieveMultiCell(lineInfo.GetAddress());
    //        if (multicell == nullptr) break;
    //        clipboardBuf += QString("%1\n").arg(QString::fromStdString(multicell->ToString()));
    //        break;
    //      }

    //    case LineInformation::LabelLineType:
    //      {
    //        medusa::Label lbl = _db->GetLabelFromAddress(lineInfo.GetAddress());
    //        if (lbl.GetType() == medusa::Label::Unknown) break;
    //        clipboardBuf += QString("%1:\n").arg(QString::fromStdString(lbl.GetLabel()));
    //        break;
    //      }

    //    case LineInformation::XrefLineType:
    //      {
    //        medusa::Address::List RefAddrList;
    //        _db->GetXRefs().From(lineInfo.GetAddress(), RefAddrList);
    //        clipboardBuf += QString::fromUtf8(";:xref");

    //        std::for_each(std::begin(RefAddrList), std::end(RefAddrList), [&](medusa::Address const& rRefAddr)
    //        {
    //          clipboardBuf += QString(" ") + (rRefAddr < lineInfo.GetAddress() ? QString::fromUtf8("\xe2\x86\x91") : QString::fromUtf8("\xe2\x86\x93")) + QString::fromStdString(rRefAddr.ToString());
    //        });
    //        clipboardBuf += "\n";
    //        break;
    //      }

    //    case LineInformation::MemoryAreaLineType:
    //      {
    //        medusa::MemoryArea const * memArea = _db->GetMemoryArea(lineInfo.GetAddress());
    //        if (memArea == nullptr) break;
    //        clipboardBuf += QString("%1\n").arg(QString::fromStdString(memArea->ToString()));
    //        break;
    //      }

    //    default: break;
    //    }
    //  }
    //  int clipboardLen = clipboardBuf.length() - leftOffLine;

    //  int lastLineOff = clipboardBuf.lastIndexOf("\n", -2);
    //  int lastLineLen = clipboardBuf.length() - lastLineOff;
    //  clipboardLen -= (lastLineLen - rightOffLine);

    //  clipboardBuf = clipboardBuf.mid(leftOffLine, clipboardLen);
    //  if (!clipboardBuf.isEmpty())
    //    clipboard->setText(clipboardBuf);
    //} while (0);
  }
}

void DisassemblyView::resizeEvent(QResizeEvent *event)
{
  QAbstractScrollArea::resizeEvent(event);
  int w, h;
  QRect rect = viewport()->rect();
  w = rect.width() / _wChar + 1;
  h = rect.height() / _hChar + 1;
  Resize(w, h);
  emit viewUpdated();
}

void DisassemblyView::setCursorPosition(QMouseEvent * evt)
{
  int xCursor = evt->x() / _wChar + horizontalScrollBar()->value();
  int yCursor = evt->y() / _hChar + verticalScrollBar()->value();

  medusa::Address addr;
  if (convertMouseToAddress(evt, addr))
    _cursorAddress = addr;

  if (xCursor > _addrLen)
  {
    _xCursor = xCursor;
  }
  _cursorTimer.start();
  _cursorBlink = false;
  updateCursor();
  ensureCursorIsVisible();
}

void DisassemblyView::setCursorPosition(int x, int y)
{
  if (x < 0)
    x = _xCursor;
  if (x >= horizontalScrollBar()->maximum())
    x = horizontalScrollBar()->maximum() - 1;
  _xCursor = x;

  medusa::Address addr;
  if (m_rDoc.MoveAddress(_cursorAddress, addr, y))
    _cursorAddress = addr;

  _cursorTimer.start();
  _cursorBlink = false;
  updateCursor();
  ensureCursorIsVisible();
}

void DisassemblyView::moveCursorPosition(int x, int y)
{
  x += _xCursor;
  setCursorPosition(x, y); // TODO: y is an offset but x is absolute
}

void DisassemblyView::resetSelection(void)
{
  _begSelectionOffset = _endSelectionOffset = _xCursor;
  _needRepaint = true; // TODO: selectionChanged
}

void DisassemblyView::setSelection(int x, int y)
{
  setCursorPosition(x, y);

  if ( x >= _addrLen
    && x < verticalScrollBar()->maximum())
    _endSelectionOffset = x;

  if ( y >= 0
    && y < horizontalScrollBar()->maximum())
    _endSelection = y;

  _needRepaint = true; // TODO: selectionChanged
}

void DisassemblyView::getSelectedAddresses(medusa::Address::List& addresses)
{
  //for (auto curSelection = _begSelection; curSelection < _endSelection; ++curSelection)
  //{
  //  medusa::View::LineInformation lineInfo;

  //  if (!_db->GetView().GetLineInformation(curSelection, lineInfo)) break;
  //  addresses.push_back(lineInfo.GetAddress());
  //}
}

void DisassemblyView::moveSelection(int x, int y)
{
  moveCursorPosition(x, y);
  _endSelectionOffset += x;
  _endSelection       += y;

  _needRepaint = true; // TODO: selectionChanged
}

void DisassemblyView::updateScrollbars(void)
{
}

bool DisassemblyView::convertPositionToAddress(QPoint const & pos, medusa::Address & addr)
{
  return GetAddressFromPosition(addr, pos.x() / _wChar, pos.y() / _hChar);
}

bool DisassemblyView::convertMouseToAddress(QMouseEvent * evt, medusa::Address & addr)
{
  return convertPositionToAddress(evt->pos(), addr);
}

void DisassemblyView::ensureCursorIsVisible(void)
{
  {
    boost::mutex::scoped_lock Lock(m_Mutex);
    if (_cursorAddress >= m_VisiblesAddresses.front() && _cursorAddress <= m_VisiblesAddresses.back())
      return;
    _Prepare(_cursorAddress);
  }
  viewUpdated();
}