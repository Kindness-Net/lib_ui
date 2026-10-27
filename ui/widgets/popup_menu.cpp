// This file is part of Desktop App Toolkit,
// a set of libraries for developing nice desktop applications.
//
// For license and copyright information please follow this link:
// https://github.com/desktop-app/legal/blob/master/LEGAL
//
#include "ui/widgets/popup_menu.h"

#include "base/platform/base_platform_info.h"
#include "base/invoke_queued.h"
#include "base/weak_qptr.h"
#include "ui/image/image_prepare.h"
#include "ui/platform/ui_platform_utility.h"
#include "ui/widgets/shadow.h"
#include "ui/widgets/menu/menu_item_base.h"
#include "ui/widgets/scroll_area.h"
#include "ui/wrap/padding_wrap.h"
#include "ui/delayed_activation.h"
#include "ui/painter.h"
#include "ui/integration.h"
#include "ui/screen_reader_mode.h"
#include "base/timer.h"
#include "ui/ui_utility.h"

#include <QtGui/QtEvents>
#include <QtGui/QCursor>
#include <QtGui/QPainter>
#include <QtGui/QScreen>
#include <QtGui/QWindow>
#include <QtWidgets/QApplication>
#include <QtWidgets/QGraphicsEffect>
#include <qpa/qplatformwindow_p.h>

namespace Ui {
namespace {

constexpr auto kSubmenuAimDelay = crl::time(300);

void ConstrainEmbeddedMenu(not_null<PopupMenu*> menu) {
	if (menu->isWindow()) {
		return;
	}
	const auto available = menu->parentWidget()->contentsRect();
	menu->move(std::clamp(menu->x(), available.left(), std::max(available.left(),
		available.x() + available.width() - menu->width())),
		std::clamp(menu->y(), available.top(), std::max(available.top(),
			available.y() + available.height() - menu->height())));
}

// 原生子窗口（如网页视图）总盖在宿主的绘制层之上，嵌入的菜单会被遮住。
[[nodiscard]] bool HasNativeChildWindow(not_null<QWidget*> window) {
	const auto handle = window->windowHandle();
	return handle && !handle->findChildren<QWindow*>().isEmpty();
}

class MenuSurface final : public QGraphicsEffect {
public:
	explicit MenuSurface(int radius) : _radius(radius) {
	}

protected:
	void draw(QPainter *p) override {
		auto offset = QPoint();
		const auto source = sourcePixmap(Qt::LogicalCoordinates, &offset, NoPad);
		auto surface = QPixmap(source.size());
		surface.setDevicePixelRatio(source.devicePixelRatio());
		surface.fill(Qt::transparent);
		{
			auto painter = QPainter(&surface);
			painter.setRenderHint(QPainter::Antialiasing);
			painter.setPen(Qt::NoPen);
			painter.setBrush(Qt::white);
			painter.drawRoundedRect(QRectF(QPointF(),
				QSizeF(source.size()) / source.devicePixelRatio()), _radius, _radius);
			painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
			painter.drawPixmap(0, 0, source);
		}
		p->drawPixmap(offset, surface);
	}

private:
	const int _radius;
};

[[nodiscard]] bool PointInTriangle(
		QPoint point,
		QPoint apex,
		QPoint from,
		QPoint till) {
	const auto side = [](QPoint a, QPoint b, QPoint c) {
		return (int64(a.x()) - c.x()) * (int64(b.y()) - c.y())
			- (int64(b.x()) - c.x()) * (int64(a.y()) - c.y());
	};
	const auto first = side(point, apex, from);
	const auto second = side(point, from, till);
	const auto third = side(point, till, apex);
	const auto negative = (first < 0) || (second < 0) || (third < 0);
	const auto positive = (first > 0) || (second > 0) || (third > 0);
	return !negative || !positive;
}

} // namespace

struct PopupMenu::SubmenuAim {
	explicit SubmenuAim(Fn<void()> callback) : timer(std::move(callback)) {
	}

	QAction *action = nullptr;
	QPoint apex;
	base::Timer timer;
	bool aiming = false;
};

PopupMenu::PopupMenu(QWidget *parent, const style::PopupMenu &st)
: RpWidget(parent)
, _st(st)
, _roundRect(_st.radius, _st.menu.itemBg)
, _boxShadow(_st.shadow)
, _scroll(this, st::defaultMultiSelect.scroll)
, _menu(_scroll->setOwnedWidget(
	object_ptr<PaddingWrap<Menu::Menu>>(
		_scroll.data(),
		object_ptr<Menu::Menu>(_scroll.data(), _st.menu),
		_st.scrollPadding))->entity()) {
	init();
}

PopupMenu *PopupMenu::Active() {
	if (const auto native = dynamic_cast<PopupMenu*>(QApplication::activePopupWidget())) {
		return native;
	}
	for (const auto top : QApplication::topLevelWidgets()) {
		if (!top->isVisible()) {
			continue;
		}
		const auto children = top->children();
		for (auto i = children.crbegin(); i != children.crend(); ++i) {
			if (const auto menu = dynamic_cast<PopupMenu*>(*i)
				; menu && !menu->isWindow() && menu->isVisible() && !menu->_hiding) {
				return menu;
			}
		}
	}
	return nullptr;
}

PopupMenu::PopupMenu(QWidget *parent, QMenu *menu, const style::PopupMenu &st)
: RpWidget(parent)
, _st(st)
, _roundRect(_st.radius, _st.menu.itemBg)
, _boxShadow(_st.shadow)
, _scroll(this, st::defaultMultiSelect.scroll)
, _menu(_scroll->setOwnedWidget(
	object_ptr<PaddingWrap<Menu::Menu>>(
		_scroll.data(),
		object_ptr<Menu::Menu>(_scroll.data(), menu, _st.menu),
		_st.scrollPadding))->entity()) {
	init();

	for (const auto &action : actions()) {
		if (const auto submenu = action->menu()) {
			_submenus.emplace(
				action,
				base::make_unique_q<PopupMenu>(this, submenu, st)
			).first->second->deleteOnHide(false);
		}
	}
}

void PopupMenu::init() {
	using namespace rpl::mappers;

	_scroll->setVerticalBarTopSkip(_st.radius);
	_scroll->setVerticalBarBottomSkip(_st.radius);

	Integration::Instance().forcePopupMenuHideRequests(
	) | rpl::on_next([=] {
		hideMenu(true);
	}, lifetime());

	_touchBeginCounter = Integration::Instance().touchCounterNow();

	installEventFilter(this);

	setupMenuWidget();

	setWindowFlags(Qt::WindowFlags(Qt::FramelessWindowHint)
		| Qt::BypassWindowManagerHint
		| Qt::Popup
		| Qt::NoDropShadowWindowHint);
	setMouseTracking(true);

	hide();

	setAttribute(Qt::WA_NoSystemBackground, true);

	_useTransparency = Platform::TranslucentWindowsSupported();
	if (_useTransparency) {
		setAttribute(Qt::WA_TranslucentBackground, true);
	} else {
		setAttribute(Qt::WA_TranslucentBackground, false);
		setAttribute(Qt::WA_OpaquePaintEvent, true);
	}
}

void PopupMenu::embedIntoParent() {
	const auto owner = parentWidget();
	if (_systemBackdrop
		|| !isWindow()
		|| !owner
		|| owner->window()->windowType() == Qt::Popup
		|| HasNativeChildWindow(owner->window())) {
		return;
	}
	_owner = owner;
	setParent(owner->window(), Qt::Widget);
	setAttribute(Qt::WA_TranslucentBackground, false);
	setAttribute(Qt::WA_NoSystemBackground, false);
	setAttribute(Qt::WA_OpaquePaintEvent, false);
	setFocusPolicy(Qt::StrongFocus);
	_useTransparency = true;
	_scroll->setGraphicsEffect(new MenuSurface(_st.radius));
	// Qt 5 会按不透明子控件的矩形跳过背景绘制，裁掉圆角处的阴影。
	_menu->setAttribute(Qt::WA_OpaquePaintEvent, false);
	if (_stashedContent) {
		_stashedContent->menu->setAttribute(Qt::WA_OpaquePaintEvent, false);
	}
	if (owner != parentWidget()) {
		QObject::connect(owner, &QObject::destroyed, this, [=] {
			hideMenu(true);
			deleteLater();
		});
	}
}

int PopupMenu::constrainedScrollHeight(int wanted) const {
	const auto styled = _st.maxHeight ? std::min(_st.maxHeight, wanted) : wanted;
	return isWindow()
		? styled
		: std::min(styled, std::max(1,
			parentWidget()->contentsRect().height()
				- _padding.top() - _padding.bottom()));
}

not_null<PopupMenu*> PopupMenu::ensureSubmenu(
		not_null<QAction*> action,
		const style::PopupMenu &st) {
	const auto &list = actions();
	const auto found = ranges::find(list, action) != end(list);
	if (!found && _stashedContent) {
		const auto &stashedList = _stashedContent->menu->actions();
		Assert(ranges::find(stashedList, action) != end(stashedList));
	} else {
		Assert(found);
	}

	const auto j = _submenus.find(action);
	if (j != end(_submenus)) {
		return j->second.get();
	}
	const auto result = _submenus.emplace(
		action,
		base::make_unique_q<PopupMenu>(this, st)
	).first->second.get();
	result->deleteOnHide(false);
	return result;
}

void PopupMenu::removeSubmenu(not_null<QAction*> action) {
	const auto menu = _submenus.take(action);
	if (menu && menu->get() == _activeSubmenu) {
		base::take(_activeSubmenu)->hideMenu(true);
	}
}

void PopupMenu::checkSubmenuShow() {
	if (_activeSubmenu) {
		return;
	} else if (const auto item = _menu->findSelectedAction()) {
		if (item->lastTriggeredSource() == Menu::TriggeredSource::Mouse) {
			if (_submenus.contains(item->action())) {
				item->setClicked(Menu::TriggeredSource::Mouse);
			}
		}
	}
}

void PopupMenu::validateCompositingSupport() {
	const auto line = st::lineWidth;
	const auto &additional = _additionalMenuPadding;
	if (_systemBackdrop) {
		_padding = additional;
		_margins = _additionalMenuMargins;
	} else if (!_useTransparency) {
		_padding = QMargins(
			std::max(line, additional.left()),
			std::max(line, additional.top()),
			std::max(line, additional.right()),
			std::max(line, additional.bottom()));
		_margins = QMargins();
	} else {
		const auto ext = _boxShadow.extend();
		_padding = QMargins(
			std::max(ext.left(), additional.left()),
			std::max(ext.top(), additional.top()),
			std::max(ext.right(), additional.right()),
			std::max(ext.bottom(), additional.bottom()));
		_margins = _padding - (additional - _additionalMenuMargins);
	}
	if (isWindow()) {
		Platform::SetWindowMargins(this, _margins);
	}
	_scroll->moveToLeft(_padding.left(), _padding.top());
	handleMenuResize();
	updateRoundingOverlay();
}

void PopupMenu::updateRoundingOverlay() {
	if (_systemBackdrop || !_useTransparency || !isWindow()) {
		_roundingOverlay.destroy();
		return;
	} else if (_roundingOverlay) {
		return;
	}
	_roundingOverlay.create(this);

	sizeValue(
	) | rpl::on_next([=](QSize size) {
		_roundingOverlay->setGeometry(QRect(QPoint(), size));
	}, _roundingOverlay->lifetime());

	_roundingOverlay->paintRequest(
	) | rpl::on_next([=](QRect clip) {
		if (_inner.isEmpty()) {
			return;
		}
		auto p = QPainter(_roundingOverlay.data());
		auto hq = PainterHighQualityEnabler(p);
		p.setCompositionMode(QPainter::CompositionMode_DestinationIn);
		_roundRect.paint(p, _inner, RectPart::AllCorners);
		if (!_grabbingForPanelAnimation) {
			p.setCompositionMode(QPainter::CompositionMode_SourceOver);
			_boxShadow.paint(p, _inner, _st.radius);
		}
	}, _roundingOverlay->lifetime());

	_roundingOverlay->setAttribute(Qt::WA_TransparentForMouseEvents);
}

void PopupMenu::handleMenuResize() {
	if (_switchState) {
		return;
	}
	auto newWidth = _padding.left() + _st.scrollPadding.left() + _menu->width() + _st.scrollPadding.right() + _padding.right();
	auto newHeight = _padding.top() + _st.scrollPadding.top() + _menu->height() + _st.scrollPadding.bottom() + _padding.bottom();
	const auto wantedHeight = newHeight - _padding.top() - _padding.bottom();
	const auto scrollHeight = constrainedScrollHeight(wantedHeight);
	_scroll->resize(
		newWidth - _padding.left() - _padding.right(),
		scrollHeight);
	{
		const auto newSize = QSize(
			newWidth,
			_padding.top() + scrollHeight + _padding.bottom());
		setFixedSize(newSize);
		resize(newSize);
	}
	_inner = rect().marginsRemoved(_padding);
	if (isVisible()) {
		ConstrainEmbeddedMenu(this);
	}
}

not_null<QAction*> PopupMenu::addAction(
		base::unique_qptr<Menu::ItemBase> widget) {
	return _menu->addAction(std::move(widget));
}

not_null<QAction*> PopupMenu::addAction(
		const QString &text,
		Fn<void()> callback,
		const style::icon *icon,
		const style::icon *iconOver) {
	return _menu->addAction(text, std::move(callback), icon, iconOver);
}

not_null<QAction*> PopupMenu::addAction(
		const QString &text,
		std::unique_ptr<PopupMenu> submenu,
		const style::icon *icon,
		const style::icon *iconOver) {
	const auto action = _menu->addAction(
		text,
		std::make_unique<QMenu>(),
		icon,
		iconOver);
	const auto saved = _submenus.emplace(
		action,
		base::unique_qptr<PopupMenu>(submenu.release())
	).first->second.get();
	// 保留弹出类型，首次显示时再根据宿主决定是否嵌入窗口。
	saved->setParent(this, saved->windowFlags());
	saved->deleteOnHide(false);
	return action;
}

not_null<QAction*> PopupMenu::addSeparator(const style::MenuSeparator *st) {
	return _menu->addSeparator(st);
}

not_null<QAction*> PopupMenu::insertAction(
		int position,
		base::unique_qptr<Menu::ItemBase> widget) {
	return _menu->insertAction(position, std::move(widget));
}

void PopupMenu::removeAction(int position) {
	const auto i = _submenus.find(_menu->actions()[position]);
	if (i != end(_submenus)) {
		_submenus.erase(i);
	}
	_menu->removeAction(position);
}

void PopupMenu::clearActions() {
	_submenus.clear();
	return _menu->clearActions();
}

void PopupMenu::setTopShift(int topShift) {
	_topShift = topShift;
}

void PopupMenu::setForceWidth(int forceWidth) {
	_menu->setForceWidth(forceWidth);
	if (_stashedContent) {
		_stashedContent->menu->setForceWidth(forceWidth);
	}
}

const std::vector<not_null<QAction*>> &PopupMenu::actions() const {
	return _menu->actions();
}

bool PopupMenu::empty() const {
	return _menu->empty();
}

void PopupMenu::paintEvent(QPaintEvent *e) {
	QPainter p(this);
	if (_systemBackdrop) {
		p.setCompositionMode(QPainter::CompositionMode_Source);
		p.fillRect(e->rect(), _systemBackdropTint);
		p.setCompositionMode(QPainter::CompositionMode_SourceOver);
	}

	if (_a_show.animating()) {
		const auto progress = _a_show.value(1.);
		if (_systemBackdrop) {
			return;
		}
		const auto opacity = _a_opacity.value(_hiding ? 0. : 1.);
		const auto state = (opacity > 0.)
			? _showAnimation->paintFrame(p, 0, 0, width(), progress, opacity)
			: PanelAnimation::PaintState();
		_showStateChanges.fire({
			.opacity = state.opacity,
			.widthProgress = state.widthProgress,
			.heightProgress = state.heightProgress,
			.appearingWidth = state.width,
			.appearingHeight = state.height,
			.appearing = true,
		});
	} else if (_a_opacity.animating()) {
		if (_showAnimation) {
			PostponeCall(this, [=] { _systemShowOverlay.destroy(); });
			_showAnimation.reset();
			_showStateChanges.fire({
				.toggling = true,
			});
		}
		if (!_systemBackdrop) {
			p.setOpacity(_a_opacity.value(0.));
		}
		p.drawPixmap(0, 0, _cache);
	} else if (_hiding || isHidden()) {
		hideFinished();
	} else if (_showAnimation) {
		if (_systemBackdrop) {
			p.drawPixmap(0, 0, _cache);
		} else {
			_showAnimation->paintFrame(p, 0, 0, width(), 1., 1.);
		}
		_showAnimation.reset();
		_showStateChanges.fire({});
		PostponeCall(this, [=] {
			// 缓存画面提交后再移除文字层，交接时不露出只有材质的空帧。
			_systemShowOverlay.destroy();
			_systemShowGeometry = QRect();
			showChildren();
			_animatePhase = AnimatePhase::Shown;
			if (isWindow()) {
				Platform::AcceptAllMouseInput(this);
			}
		});
	} else {
		paintBg(p);
	}
}

void PopupMenu::paintBg(QPainter &p) {
	if (_systemBackdrop) {
		return;
	}
	if (!isWindow()) {
		_roundRect.paint(p, _inner);
		if (!_grabbingForPanelAnimation) {
			_boxShadow.paint(p, _inner, _st.radius);
		}
	} else if (!_useTransparency) {
		p.fillRect(0, 0, width() - _padding.right(), _padding.top(), _st.shadowFallback);
		p.fillRect(width() - _padding.right(), 0, _padding.right(), height() - _padding.bottom(), _st.shadowFallback);
		p.fillRect(_padding.left(), height() - _padding.bottom(), width() - _padding.left(), _padding.bottom(), _st.shadowFallback);
		p.fillRect(0, _padding.top(), _padding.left(), height() - _padding.top(), _st.shadowFallback);
	}
}

QColor PopupMenu::contentBackground() const {
	return _systemBackdrop ? QColor(Qt::transparent) : _st.menu.itemBg->c;
}

void PopupMenu::setSystemBackdrop(bool enabled, QColor tint) {
	if (_systemBackdrop == enabled && _systemBackdropTint == tint) {
		return;
	}
	_systemBackdropTint = tint;
	if (_systemBackdrop == enabled) {
		update();
		return;
	}
	_systemBackdrop = enabled;
	if (enabled && !isWindow()) {
		const auto position = mapToGlobal(QPoint());
		qApp->removeEventFilter(this);
		setParent(_owner ? _owner.data() : parentWidget(),
			Qt::FramelessWindowHint | Qt::BypassWindowManagerHint
				| Qt::Popup | Qt::NoDropShadowWindowHint);
		setAttribute(Qt::WA_TranslucentBackground);
		setAttribute(Qt::WA_NoSystemBackground);
		_scroll->setGraphicsEffect(nullptr);
		move(position);
	}
	if (::Platform::IsWindows()) {
		// Qt 的弹出菜单已捕获键盘输入，有宿主时不转移系统激活窗口。
		setWindowFlag(Qt::WindowDoesNotAcceptFocus, enabled && parentWidget());
	}
	_menu->setAttribute(Qt::WA_OpaquePaintEvent, !enabled && isWindow()
		&& _st.menu.itemBg->c.alpha() == 255);
	if (_stashedContent) {
		_stashedContent->menu->setAttribute(Qt::WA_OpaquePaintEvent,
			_menu->testAttribute(Qt::WA_OpaquePaintEvent));
	}
	validateCompositingSupport();
	update();
}

void PopupMenu::handleActivated(const Menu::CallbackData &data) {
	if (data.source != TriggeredSource::Mouse || _submenus.empty()) {
		return;
	} else if (!popupSubmenuFromAction(data)) {
		clearSubmenuAim();
		if (const auto currentSubmenu = base::take(_activeSubmenu)) {
			currentSubmenu->hideMenu(true);
		}
	}
}

QAction *PopupMenu::activeSubmenuAction() const {
	return (_activeSubmenu && _submenuAim) ? _submenuAim->action : nullptr;
}

not_null<PopupMenu::SubmenuAim*> PopupMenu::submenuAim() {
	if (!_submenuAim) {
		_submenuAim = std::make_unique<SubmenuAim>([=] {
			if (isHidden() || _hiding) {
				return;
			}
			// Pointer stopped while aiming, let hovered item win.
			_submenuAim->aiming = false;
			_menu->setMouseSelectionFrozen(false);
			_menu->handleMouseMove(_menu->lastMouseGlobal());
		});
		watchMouseMoves();
	}
	return _submenuAim.get();
}

void PopupMenu::watchMouseMoves() {
	_menu->setMouseMovedCallback([=](QPoint globalPosition) {
		handleMouseMoved(globalPosition);
	});
}

void PopupMenu::handleMouseMoved(QPoint globalPosition) {
	if (_parent
		&& QRect(mapToGlobal(_inner.topLeft()), _inner.size()).contains(
			globalPosition)) {
		// Pointer reached this submenu, not aiming at it anymore.
		_parent->clearSubmenuAim();
	}
	if (!_activeSubmenu || !_submenuAim) {
		return;
	}
	const auto aim = _submenuAim.get();
	const auto owner = aim->action
		? _menu->itemForAction(aim->action)
		: nullptr;
	if (owner
		&& QRect(owner->mapToGlobal(QPoint()), owner->size()).contains(
			globalPosition)) {
		aim->apex = globalPosition;
		aim->aiming = true;
		aim->timer.cancel();
		_menu->setMouseSelectionFrozen(false);
	} else if (aim->aiming
		&& !QGuiApplication::mouseButtons()
		&& insideSubmenuAim(globalPosition)) {
		_menu->setMouseSelectionFrozen(true);
		aim->timer.callOnce(kSubmenuAimDelay);
	} else {
		clearSubmenuAim();
	}
}

bool PopupMenu::insideSubmenuAim(QPoint position) const {
	const auto submenu = QRect(
		_activeSubmenu->mapToGlobal(_activeSubmenu->inner().topLeft()),
		_activeSubmenu->inner().size());
	if (submenu.isEmpty()) {
		return false;
	}
	const auto mine = QRect(mapToGlobal(_inner.topLeft()), _inner.size());
	const auto opensRight = (submenu.center().x() >= mine.center().x());
	const auto edge = opensRight ? submenu.left() : submenu.right();
	const auto beyond = opensRight
		? (position.x() >= edge)
		: (position.x() <= edge);
	if (beyond && !mine.contains(position)) {
		// Menus overlap by shadow width, last pixels belong to this menu.
		// Compositor owned positions may overlap much more than that.
		return (position.y() >= submenu.top())
			&& (position.y() <= submenu.bottom());
	}
	return PointInTriangle(
		position,
		_submenuAim->apex,
		QPoint(edge, submenu.top()),
		QPoint(edge, submenu.bottom()));
}

void PopupMenu::clearSubmenuAim() {
	if (_submenuAim) {
		_submenuAim->aiming = false;
		_submenuAim->timer.cancel();
		_menu->setMouseSelectionFrozen(false);
	}
}

void PopupMenu::handleTriggered(const Menu::CallbackData &data) {
	if (!popupSubmenuFromAction(data)) {
		_triggering = true;
		if (!data.preventClose) {
			hideMenu();
		}
		auto weak = base::make_weak(this);
		data.action->trigger();
		if (weak) {
			_triggering = false;
			if (_deleteLater) {
				_deleteLater = false;
				deleteLater();
			}
		}
	}
}

bool PopupMenu::popupSubmenuFromAction(const Menu::CallbackData &data) {
	if (!data.action) {
		return false;
	}
	if (const auto i = _submenus.find(data.action); i != end(_submenus)) {
		const auto submenu = i->second.get();
		if (_activeSubmenu != submenu) {
			popupSubmenu(data.action, submenu, data.actionTop, data.source);
		}
		return true;
	}
	return false;
}

void PopupMenu::popupSubmenu(
		not_null<QAction*> action,
		not_null<PopupMenu*> submenu,
		int actionTop,
		TriggeredSource source) {
	clearSubmenuAim();
	if (auto currentSubmenu = base::take(_activeSubmenu)) {
		currentSubmenu->hideMenu(true);
	}
	if (submenu) {
		const auto padding = _useTransparency
			? _boxShadow.extend()
			: QMargins(st::lineWidth, 0, st::lineWidth, 0);
		QPoint p(_inner.x() + (style::RightToLeft() ? padding.right() : (_inner.width() - padding.left())), _inner.y() + actionTop);
		_activeSubmenu = submenu;
		_activeSubmenu->menu()->clearSelection();
		_activeSubmenu->setAccessibleName(action->text());
		if (_activeSubmenu->prepareGeometryFor(
				mapToGlobal(p),
				this,
				_menu->itemForAction(action))) {
			_activeSubmenu->animatePhaseValue(
			) | rpl::filter([](AnimatePhase phase) {
				return (phase == AnimatePhase::Shown);
			}) | rpl::take(1) | rpl::on_next([submenu = _activeSubmenu] {
				submenu->menu()->clearSelection();
			}, _activeSubmenu->lifetime());
			// showPrepared() reaches the platform window, deep enough for
			// the owner to destroy us from inside it.
			const auto weak = base::make_weak(this);
			_activeSubmenu->showPrepared(source);
			if (!weak) {
				return;
			}
			_menu->setChildShownAction(action);
			const auto aim = submenuAim();
			aim->action = action;
			aim->aiming = (source == TriggeredSource::Mouse);
			aim->apex = _menu->lastMouseGlobal();
		} else {
			_activeSubmenu = nullptr;
		}
	}
}

void PopupMenu::forwardKeyPress(not_null<QKeyEvent*> e) {
	if (!handleKeyPress(e->key())) {
		_menu->handleKeyPress(e);
	}
}

bool PopupMenu::handleKeyPress(int key) {
	// Keyboard takes over, pending mouse aim must not switch items.
	clearSubmenuAim();
	if (_activeSubmenu) {
		_activeSubmenu->handleKeyPress(key);
		return true;
	} else if (key == Qt::Key_Escape) {
		hideMenu(_parent ? true : false);
		return true;
	} else if (key == (style::RightToLeft() ? Qt::Key_Right : Qt::Key_Left)) {
		if (_parent) {
			hideMenu(true);
			return true;
		}
	} else if (key == (style::RightToLeft() ? Qt::Key_Left : Qt::Key_Right)) {
		if (const auto item = _menu->findSelectedAction()) {
			if (_submenus.contains(item->action())) {
				item->setClicked(Menu::TriggeredSource::Keyboard);
			}
		}
	}
	return false;
}

void PopupMenu::handleMouseMove(QPoint globalPosition) {
	if (_parent) {
		_parent->forwardMouseMove(globalPosition);
	}
}

void PopupMenu::handleMousePress(QPoint globalPosition) {
	if (_parent) {
		_parent->forwardMousePress(globalPosition);
	} else {
		hideMenu();
	}
}

void PopupMenu::handleMouseRelease(QPoint globalPosition) {
	if (_parent) {
		_parent->forwardMouseRelease(globalPosition);
	} else {
		hideMenu();
	}
}

void PopupMenu::focusOutEvent(QFocusEvent *e) {
	if (isWindow() && !InFocusChain(this)) {
		hideMenu();
	}
}

void PopupMenu::hideEvent(QHideEvent *e) {
	if (!isWindow() && !_parent) {
		qApp->removeEventFilter(this);
		if (const auto previous = base::take(_previousFocus)) {
			if (previous->isVisible() && previous->isEnabled()) {
				previous->setFocus(Qt::PopupFocusReason);
			}
		}
	}
	if (_deleteOnHide) {
		if (_triggering) {
			_deleteLater = true;
		} else {
			deleteLater();
		}
	}
}

void PopupMenu::keyPressEvent(QKeyEvent *e) {
	forwardKeyPress(e);
}

void PopupMenu::mouseMoveEvent(QMouseEvent *e) {
	forwardMouseMove(e->globalPos());
}

void PopupMenu::mousePressEvent(QMouseEvent *e) {
	// Mouse presses, synthesized from touch events,
	// should be ignored, if the touch, that caused
	// them, started before the menu was created.
	if (e->source() != Qt::MouseEventSynthesizedBySystem
		|| (Integration::Instance().touchCounterNow()
			> _touchBeginCounter)) {
		forwardMousePress(e->globalPos());
	}
}

bool PopupMenu::eventFilter(QObject *o, QEvent *e) {
	const auto type = e->type();
	if (!isWindow() && !_parent && isVisible() && !_hiding) {
		if (o == window()
			&& (type == QEvent::WindowDeactivate
				|| type == QEvent::Resize
				|| type == QEvent::Hide)) {
			hideMenu(true);
			return false;
		}
		const auto widget = qobject_cast<QWidget*>(o);
		auto inside = false;
		for (auto menu = this; menu; menu = menu->_activeSubmenu.data()) {
			if (widget == menu || (widget && menu->isAncestorOf(widget))) {
				inside = true;
				break;
			}
		}
		if (!inside && widget && widget->window() == window()) {
			if (type == QEvent::MouseButtonPress
				|| type == QEvent::MouseButtonDblClick
				|| type == QEvent::TouchBegin) {
				hideMenu();
				return true;
			} else if (type == QEvent::KeyPress) {
				forwardKeyPress(static_cast<QKeyEvent*>(e));
				return true;
			}
		}
	}
	if (type == QEvent::TouchBegin
		|| type == QEvent::TouchUpdate
		|| type == QEvent::TouchEnd) {
		if (o == windowHandle() && isActiveWindow()) {
			e->setAccepted(
				_touchForward.handle(this, static_cast<QTouchEvent*>(e)));
			return e->isAccepted();
		}
	}
	return false;
}

void PopupMenu::hideMenu(bool fast) {
	if (isHidden() || (_hiding && !fast)) {
		return;
	}
	if (_parent && !_a_opacity.animating()) {
		_parent->childHiding(this);
	}
	if (fast) {
		hideFast();
	} else {
		hideAnimated();
		if (_parent) {
			_parent->hideMenu();
		}
	}
	if (_activeSubmenu) {
		_activeSubmenu->hideMenu(fast);
	}
}

void PopupMenu::childHiding(PopupMenu *child) {
	if (_activeSubmenu && _activeSubmenu == child) {
		_activeSubmenu = nullptr;
	}
	if (!_activeSubmenu) {
		clearSubmenuAim();
		if (_submenuAim) {
			_submenuAim->action = nullptr;
		}
		_menu->setChildShownAction(nullptr);
	}
	if (!_hiding && !isHidden()) {
		raise();
		if (isWindow()) {
			if (!windowFlags().testFlag(Qt::WindowDoesNotAcceptFocus)) {
				activateWindow();
			}
		} else {
			setFocus(Qt::PopupFocusReason);
		}
	}
}

void PopupMenu::setOrigin(PanelAnimation::Origin origin) {
	_origin = _forcedOrigin.value_or(origin);
}

void PopupMenu::setForcedOrigin(PanelAnimation::Origin origin) {
	_forcedOrigin = origin;
}

void PopupMenu::setForcedVerticalOrigin(VerticalOrigin origin) {
	_forcedVerticalOrigin = origin;
}

void PopupMenu::setAdditionalMenuPadding(
		QMargins padding,
		QMargins margins) {
	Expects(padding.left() >= margins.left()
		&& padding.right() >= margins.right()
		&& padding.top() >= margins.top()
		&& padding.bottom() >= margins.bottom());

	if (_additionalMenuPadding != padding
		|| _additionalMenuMargins != margins) {
		_additionalMenuPadding = padding;
		_additionalMenuMargins = margins;
		_roundingOverlay.destroy();
	}
}

void PopupMenu::hideAnimated() {
	if (isHidden() || _hiding) {
		return;
	}
	if (!_keepingDelayedActivationPaused) {
		_keepingDelayedActivationPaused = true;
		KeepDelayedActivationPaused(true);
	}
	startHideAnimation();
}

void PopupMenu::hideFast() {
	if (isHidden()) return;

	_a_opacity.stop();
	hideFinished();
}

void PopupMenu::hideFinished() {
	_hiding = false;
	_a_show.stop();
	_systemShowOverlay.destroy();
	_cache = QPixmap();
	_animatePhase = AnimatePhase::Hidden;
	if (!isHidden()) {
		hide();
	}
	if (!_systemShowGeometry.isEmpty()) {
		const auto geometry = base::take(_systemShowGeometry);
		setFixedSize(geometry.size());
		move(geometry.topLeft());
		_showAnimation.reset();
	}
	if (_systemBackdrop) {
		setWindowOpacity(1.);
		finishSwitchAnimation();
	}
}

void PopupMenu::prepareCache() {
	if (_a_opacity.animating()) return;
	if (_systemBackdrop && _showAnimation) {
		return;
	}
	if (_systemBackdrop && _switchState) {
		// 收起时冻结当前换页画面，窗口大小和内容缓存保持一致。
		_cache = Ui::PixmapFromImage(grabForPanelAnimation());
		_switchState->animation.stop();
		return;
	}

	auto showAnimation = base::take(_a_show);
	auto showAnimationData = base::take(_showAnimation);
	if (showAnimation.animating() || _systemBackdrop) {
		_showStateChanges.fire({});
	}
	showChildren();
	_cache = _systemBackdrop
		? Ui::PixmapFromImage(grabForPanelAnimation())
		: GrabWidget(this);
	_showAnimation = base::take(showAnimationData);
	_a_show = base::take(showAnimation);
	if (_a_show.animating()) {
		fireCurrentShowState();
	}
}

void PopupMenu::startHideAnimation() {
	if (!_useTransparency) {
		_a_opacity.stop();
		_hiding = true;
		InvokeQueued(this, [=] {
			if (_hiding) {
				hideFinished();
			}
		});
		return;
	}
	_hiding = false;
	prepareCache();
	_hiding = true;
	_animatePhase = AnimatePhase::StartHide;
	if (_systemBackdrop) {
		_showStateChanges.fire({ .toggling = true });
	}
	hideChildren();
	_a_opacity.start(
		[=] { opacityAnimationCallback(); },
		1.,
		0.,
		_st.duration);
	if (_systemBackdrop) {
		setWindowOpacity(_a_opacity.value(0.));
	}
}

void PopupMenu::startShowAnimation() {
	if (!_useTransparency) {
		_a_show.stop();
		update();
		return;
	}
	if (!_a_show.animating()) {
		auto opacityAnimation = base::take(_a_opacity);
		if (_systemBackdrop) {
			_systemShowGeometry = geometry();
			_showStateChanges.fire({});
		}
		showChildren();
		auto cache = grabForPanelAnimation();
		if (_systemBackdrop) {
			_cache = Ui::PixmapFromImage(QImage(cache));
			// 文字层固定在最终位置，避免原生窗口移动先于纹理提交造成错位。
			_systemShowOverlay.create(this);
			_systemShowOverlay->setWindowFlags(Qt::Tool
				| Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint
				| Qt::WindowDoesNotAcceptFocus | Qt::WindowTransparentForInput);
			_systemShowOverlay->setAttribute(Qt::WA_TranslucentBackground);
			_systemShowOverlay->setAttribute(Qt::WA_ShowWithoutActivating);
			_systemShowOverlay->setFixedSize(_systemShowGeometry.size());
			_systemShowOverlay->move(_systemShowGeometry.topLeft());
			Integration::Instance().preparePopupMenu(_systemShowOverlay.data());
			_systemShowOverlay->paintRequest(
			) | rpl::on_next([=](QRect) {
				auto p = QPainter(_systemShowOverlay);
				const auto opacity = _showAnimation
					? _showAnimation->computeState(_a_show.value(1.), 1.).opacity
					: 1.;
				p.setClipRect(QRect(pos() - _systemShowGeometry.topLeft(), size()));
				p.setOpacity(opacity * _a_opacity.value(_hiding ? 0. : 1.));
				p.drawPixmap(0, 0, _cache);
			}, _systemShowOverlay->lifetime());
		}
		_a_opacity = base::take(opacityAnimation);

		const auto pixelRatio = style::DevicePixelRatio();
		_showAnimation = std::make_unique<PanelAnimation>(_st.animation, _origin);
		_showAnimation->setSkipShadow(_systemBackdrop);
		_showAnimation->setFinalImage(std::move(cache), QRect(_inner.topLeft() * pixelRatio, _inner.size() * pixelRatio), _st.radius);
		if (!_systemBackdrop) {
			_showAnimation->setCornerMasks(Images::CornersMask(_st.radius));
		}
		_showAnimation->start();
	}
	_animatePhase = AnimatePhase::StartShow;
	hideChildren();
	_a_show.start([this] { showAnimationCallback(); }, 0., 1., _st.showDuration);
	fireCurrentShowState();
	updateSystemBackdropGeometry();
}

void PopupMenu::fireCurrentShowState() {
	if (_systemBackdrop) {
		_showStateChanges.fire({ .toggling = true });
		return;
	}
	const auto state = _showAnimation->computeState(
		_a_show.value(1.),
		_a_opacity.value(1.));
	_showStateChanges.fire({
		.opacity = state.opacity,
		.widthProgress = state.widthProgress,
		.heightProgress = state.heightProgress,
		.appearingWidth = state.width,
		.appearingHeight = state.height,
		.appearing = true,
	});
}

void PopupMenu::opacityAnimationCallback() {
	if (_systemBackdrop) {
		// 整窗淡出包含材质和阴影，内容不再单独乘一次透明度。
		setWindowOpacity(_a_opacity.value(0.));
		if (_systemShowOverlay) {
			_systemShowOverlay->update();
		}
	}
	update();
	if (!_a_opacity.animating()) {
		hideFinished();
	}
}

void PopupMenu::showAnimationCallback() {
	updateSystemBackdropGeometry();
	update();
	if (_systemShowOverlay && isVisible()) {
		_systemShowOverlay->show();
		_systemShowOverlay->update();
	}
}

void PopupMenu::updateSystemBackdropGeometry() {
	using Origin = PanelAnimation::Origin;

	if (!_systemBackdrop || !_showAnimation) {
		return;
	}
	// 窗口位置与尺寸一起更新，原生外框跟随展开区域。
	const auto state = _showAnimation->computeState(_a_show.value(1.), 1.);
	const auto size = QSize(
		_padding.left() + state.width + _padding.right(),
		_padding.top() + state.height + _padding.bottom());
	const auto right = (_origin == Origin::TopRight || _origin == Origin::BottomRight);
	const auto bottom = (_origin == Origin::BottomLeft || _origin == Origin::BottomRight);
	const auto position = _systemShowGeometry.topLeft() + QPoint(
		right ? _systemShowGeometry.width() - size.width() : 0,
		bottom ? _systemShowGeometry.height() - size.height() : 0);
	setMinimumSize(0, 0);
	setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
	setGeometry(QRect(position, size));
	// QWidget 隐藏时会延迟同步原生坐标，首帧绘制前必须使两者一致。
	const auto handle = windowHandle();
	if (handle->geometry() != geometry()) {
		handle->setGeometry(geometry());
	}
}

QImage PopupMenu::grabForPanelAnimation() {
	SendPendingMoveResizeEvents(this);
	const auto pixelRatio = style::DevicePixelRatio();
	auto result = QImage(size() * pixelRatio, QImage::Format_ARGB32_Premultiplied);
	result.setDevicePixelRatio(pixelRatio);
	result.fill(Qt::transparent);
	{
		QPainter p(&result);
		_grabbingForPanelAnimation = true;
		p.fillRect(_inner, contentBackground());
		for (const auto child : children()) {
			if (const auto widget = qobject_cast<QWidget*>(child)) {
				// Submenus are windows of their own, they are not a part
				// of what this menu paints, and their pos() is meaningless
				// in our coordinates.
				if (!widget->isWindow() && !widget->isHidden()) {
					RenderWidget(p, widget, widget->pos());
				}
			}
		}
		_grabbingForPanelAnimation = false;
	}
	return result;
}

void PopupMenu::deleteOnHide(bool del) {
	_deleteOnHide = del;
}

QPoint PopupMenu::ConstrainToParentScreen(
		not_null<PopupMenu*> menu,
		QPoint globalPos) {
	auto result = globalPos;
	if (const auto parent = menu->parentWidget()) {
		if (const auto parentScreen = parent->window()->screen()) {
			const auto r = parentScreen->availableGeometry();
			if (!r.contains(result)) {
				result.setX(std::clamp(result.x(), r.left(), r.right()));
				result.setY(std::clamp(result.y(), r.top(), r.bottom()));
			}
		}
	}
	return result;
}

void PopupMenu::popup(const QPoint &p) {
	if (!empty() && prepareGeometryFor(p)) {
		popupPrepared();
		return;
	}
	_hiding = false;
	_a_opacity.stop();
	_a_show.stop();
	_cache = QPixmap();
	hide();
	if (_deleteOnHide) {
		deleteLater();
	}
}

void PopupMenu::popupPrepared() {
	showPrepared(TriggeredSource::Mouse);
}

PanelAnimation::Origin PopupMenu::preparedOrigin() const {
	return _origin;
}

QMargins PopupMenu::preparedPadding() const {
	return _padding;
}



QMargins PopupMenu::additionalMenuPadding() const {
	return _additionalMenuPadding;
}

QMargins PopupMenu::additionalMenuMargins() const {
	return _additionalMenuMargins;
}

QMargins PopupMenu::preparedMargins() const {
	return _margins;
}

bool PopupMenu::useTransparency() const {
	return _useTransparency;
}

int PopupMenu::scrollTop() const {
	return _scroll->scrollTop();
}

rpl::producer<int> PopupMenu::scrollTopValue() const {
	return _scroll->scrollTopValue();
}

rpl::producer<PopupMenu::ShowState> PopupMenu::showStateValue() const {
	return _showStateChanges.events();
}

bool PopupMenu::prepareGeometryFor(const QPoint &p) {
	return prepareGeometryFor(p, nullptr, nullptr);
}

bool PopupMenu::prepareGeometryFor(
		const QPoint &p,
		PopupMenu *parent,
		QWidget *parentActionWidget) {
	if (_clearLastSeparator) {
		_menu->clearLastSeparator();
		for (const auto &[action, submenu] : _submenus) {
			submenu->menu()->clearLastSeparator();
		}
	}

	_parent = parent;
	if (_parent) {
		watchMouseMoves();
	}
	const auto screen = QGuiApplication::screenAt(p);

	Integration::Instance().preparePopupMenu(this);
	embedIntoParent();
	if (isWindow()) {
		createWinId();
		windowHandle()->removeEventFilter(this);
		windowHandle()->installEventFilter(this);
		if (_parent) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
			setScreen(_parent->screen());
#else // Qt >= 6.0.0
			windowHandle()->setScreen(_parent->screen());
#endif // Qt < 6.0.0
		} else if (screen) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
			setScreen(screen);
#else // Qt >= 6.0.0
			windowHandle()->setScreen(screen);
#endif // Qt < 6.0.0
		}
	}
	validateCompositingSupport();

	using Origin = PanelAnimation::Origin;
	auto origin = Origin::TopLeft;
	const auto forceLeft = _forcedOrigin
		&& (*_forcedOrigin == Origin::TopLeft
			|| *_forcedOrigin == Origin::BottomLeft);
	const auto forceTop = (_forcedVerticalOrigin
		&& (*_forcedVerticalOrigin == VerticalOrigin::Top))
		|| (_forcedOrigin
			&& (*_forcedOrigin == Origin::TopLeft
				|| *_forcedOrigin == Origin::TopRight));
	const auto forceRight = _forcedOrigin
		&& (*_forcedOrigin == Origin::TopRight
			|| *_forcedOrigin == Origin::BottomRight);
	const auto forceBottom = (_forcedVerticalOrigin
		&& (*_forcedVerticalOrigin == VerticalOrigin::Bottom))
		|| (_forcedOrigin
			&& (*_forcedOrigin == Origin::BottomLeft
				|| *_forcedOrigin == Origin::BottomRight));
	auto w = p - QPoint(
		std::max(
			_additionalMenuPadding.left() - _boxShadow.extend().left(),
			0),
		_padding.top() - _topShift);
	auto r = !isWindow()
		? QRect(parentWidget()->mapToGlobal(parentWidget()->contentsRect().topLeft()),
			parentWidget()->contentsRect().size())
		: screen ? screen->availableGeometry() : QRect();
#if QT_VERSION >= QT_VERSION_CHECK(6, 11, 0) && defined QT_FEATURE_wayland && QT_CONFIG(wayland)
	using namespace QNativeInterface::Private;
	if (const auto native
			= isWindow() ? windowHandle()->nativeInterface<QWaylandWindow>() : nullptr) {
		const auto padding = _additionalMenuPadding - _additionalMenuMargins;
		base::take(r);
		if (_parent) {
			// we must have an action to position the submenu around
			Assert(parentActionWidget != nullptr);
			native->setParentControlGeometry(
				QRect(
					parentActionWidget->mapTo(
						parentActionWidget->window(),
						QPoint()),
					parentActionWidget->size())
				+ _st.scrollPadding);
		} else if (padding.top()) {
			// provide the compositor with a range for flip_y so it uses
			// the cursor point instead of the padding's top point
			native->setParentControlGeometry(
				QRect(
					p
						- parentWidget()->window()->pos()
						- QPoint(padding.left(), padding.top()),
					QSize(1, padding.top())));
			windowHandle()->setProperty(
				"_q_waylandPopupAnchor",
				QVariant::fromValue(Qt::TopEdge | Qt::LeftEdge));
		}
		native->setExtendedWindowType(_parent
			? QWaylandWindow::SubMenu
			: QWaylandWindow::Menu);
	}
#endif // Qt >= 6.11.0 && wayland
	if (_parent) {
		const auto parentInner = QRect(
			_parent->mapToGlobal(_parent->inner().topLeft()),
			_parent->inner().size());
		// 菜单主体贴边；阴影可以相交，不能用阴影边距缩短展开距离。
		const auto left = parentInner.x() - _inner.width() - _inner.x();
		const auto right = parentInner.x() + parentInner.width() - _inner.x();
		const auto fits = [&](int x) {
			return r.isNull()
				|| (x + (isWindow() ? _margins.left() : 0) >= r.x()
					&& x + width() - (isWindow() ? _margins.right() : 0)
						<= r.x() + r.width());
		};
		auto openLeft = forceRight || (!forceLeft && style::RightToLeft());
		if (!forceLeft && !forceRight
			&& !fits(openLeft ? left : right)
			&& fits(openLeft ? right : left)) {
			openLeft = !openLeft;
		}
		w.setX(openLeft ? left : right);
		origin = openLeft ? Origin::TopRight : Origin::TopLeft;
	} else if (style::RightToLeft()) {
		const auto badLeft = !r.isNull() && w.x() - width() < r.x() - _margins.left();
		if (forceRight || (badLeft && !forceLeft)) {
			w.setX(r.x() - _margins.left());
		} else {
			w.setX(w.x() - width());
		}
	} else {
		const auto badLeft = !r.isNull() && w.x() + width() - _margins.right() > r.x() + r.width();
		if (forceRight || (badLeft && !forceLeft)) {
			w.setX(p.x() - width() + std::max(
				_additionalMenuPadding.right() - _boxShadow.extend().right(),
				0));
			origin = PanelAnimation::Origin::TopRight;
		}
	}
	const auto badTop = !r.isNull() && w.y() + height() - _margins.bottom() > r.y() + r.height();
	if (forceBottom || (badTop && !forceTop)) {
		if (_parent) {
			w.setY(r.y() + r.height() - height() + _margins.bottom());
		} else {
			w.setY(p.y() - height() + _margins.bottom());
			origin = (origin == PanelAnimation::Origin::TopRight)
				? PanelAnimation::Origin::BottomRight
				: PanelAnimation::Origin::BottomLeft;
		}
	}
	if (!r.isNull()) {
		if (w.x() + width() - _margins.right() > r.x() + r.width()) {
			w.setX(r.x() + r.width() + _margins.right() - width());
		}
		if (w.x() + _margins.left() < r.x()) {
			w.setX(r.x() - _margins.left());
		}
		if (w.y() + height() - _margins.bottom() > r.y() + r.height()) {
			w.setY(r.y() + r.height() + _margins.bottom() - height());
		}
		if (w.y() + _margins.top() < r.y()) {
			w.setY(r.y() - _margins.top());
		}
	}
	move(isWindow() ? w : parentWidget()->mapFromGlobal(w));

	setOrigin(origin);
	return true;
}

void PopupMenu::showPrepared(TriggeredSource source) {
	if (!isWindow() && !_parent) {
		if (auto previous = Active(); previous && previous != this) {
			while (previous->_parent) {
				previous = previous->_parent;
			}
			previous->hideMenu(true);
		}
		_previousFocus = QApplication::focusWidget();
		qApp->installEventFilter(this);
	}
	ConstrainEmbeddedMenu(this);
	startShowAnimation();

	if (isWindow() && ::Platform::IsWindows()) {
		ForceFullRepaintSync(this);
	}
	Integration::Instance().preparePopupMenu(this);
	// show() reaches the platform window deeply enough for the owner to destroy
	// this menu, so nothing below may touch it without checking its lifetime.
	const auto weak = base::make_weak(this);
	show();
	if (!weak) {
		return;
	}
	raise();
	if (isWindow()) {
		Platform::ShowOverAll(this);
		if (!windowFlags().testFlag(Qt::WindowDoesNotAcceptFocus)) {
			activateWindow();
		}
	} else {
		setFocus(Qt::PopupFocusReason);
	}
	if (Ui::ScreenReaderModeActive()) {
		_menu->setShowSource(TriggeredSource::Keyboard);
	} else {
		_menu->setShowSource(source);
	}
}

void PopupMenu::setClearLastSeparator(bool clear) {
	_clearLastSeparator = clear;
}


void PopupMenu::finishSwitchAnimation() {
	if (!_switchState) {
		return;
	}
	const auto positionShift = _switchState->positionShift;
	const auto baseY = _switchState->baseY;
	_switchState->overlay.destroy();
	_switchState.reset();
	_scroll->show();
	handleMenuResize();
	if (positionShift) {
		move(x(), baseY + positionShift);
	}
}

void PopupMenu::setupMenuWidget() {
	if (_systemBackdrop || _scroll->graphicsEffect()) {
		_menu->setAttribute(Qt::WA_OpaquePaintEvent, false);
	}
	const auto paddingWrap = static_cast<PaddingWrap<Menu::Menu>*>(
		_menu->parentWidget());
	style::PaletteChanged(
	) | rpl::on_next([=] {
		paddingWrap->update();
		update();
	}, paddingWrap->lifetime());

	paddingWrap->paintRequest(
	) | rpl::on_next([=](QRect clip) {
		const auto top = clip.intersected(
			QRect(0, 0, paddingWrap->width(), _st.scrollPadding.top()));
		const auto bottom = clip.intersected(QRect(
			0,
			paddingWrap->height() - _st.scrollPadding.bottom(),
			paddingWrap->width(),
			_st.scrollPadding.bottom()));
		auto p = QPainter(paddingWrap);
		if (!top.isEmpty()) {
			p.fillRect(top, contentBackground());
		}
		if (!bottom.isEmpty()) {
			p.fillRect(bottom, contentBackground());
		}
	}, paddingWrap->lifetime());

	rpl::combine(
		_scroll->scrollTopValue(),
		_scroll->heightValue(),
		_menu->heightValue()
	) | rpl::on_next([=](int scrollTop, int scrollHeight, int) {
		const auto scrollBottom = scrollTop + scrollHeight;
		paddingWrap->setVisibleTopBottom(scrollTop, scrollBottom);
	}, paddingWrap->lifetime());

	_menu->scrollToRequests(
	) | rpl::on_next([=](ScrollToRequest request) {
		_scroll->scrollTo({
			request.ymin ? (_st.scrollPadding.top() + request.ymin) : 0,
			(request.ymax == _menu->height()
				? paddingWrap->height()
				: (_st.scrollPadding.top() + request.ymax)),
		});
	}, _menu->lifetime());

	_menu->resizesFromInner(
	) | rpl::on_next([=] {
		handleMenuResize();
	}, _menu->lifetime());
	_menu->setActivatedCallback([this](const Menu::CallbackData &data) {
		handleActivated(data);
	});
	_menu->setTriggeredCallback([this](const Menu::CallbackData &data) {
		handleTriggered(data);
	});
	_menu->setKeyPressDelegate([this](int key) {
		return handleKeyPress(key);
	});
	_menu->setMouseMoveDelegate([this](QPoint globalPosition) {
		handleMouseMove(globalPosition);
	});
	_menu->setMousePressDelegate([this](QPoint globalPosition) {
		handleMousePress(globalPosition);
	});
	_menu->setMouseReleaseDelegate([this](QPoint globalPosition) {
		handleMouseRelease(globalPosition);
	});
}

void PopupMenu::swapWithStashed() {
	Assert(_stashedContent != nullptr);

	// Take current content out of scroll.
	auto currentWrap = _scroll->takeWidget<QWidget>();
	auto currentMenu = _menu;

	// Restore stashed content into scroll.
	// Subscriptions and delegates are still alive on the stashed widgets.
	_scroll->setOwnedWidget(std::move(_stashedContent->wrap));
	_menu = _stashedContent->menu;

	// Stash current content.
	_stashedContent->wrap = std::move(currentWrap);
	_stashedContent->menu = currentMenu;

	// _submenus stays shared — actions from both pages use it.
}

void PopupMenu::stashContent(Fn<void(not_null<PopupMenu*>)> fillNew) {
	if (_switchState) {
		_switchState->animation.stop();
		_switchState->overlay.destroy();
		_switchState.reset();
		_scroll->show();
	}

	// Stash current content. _submenus stays shared.
	_stashedContent = std::make_unique<StashedContent>(StashedContent{
		.wrap = _scroll->takeWidget<QWidget>(),
		.menu = _menu,
	});

	// Create new menu in scroll.
	auto wrap = object_ptr<PaddingWrap<Menu::Menu>>(
		_scroll.data(),
		object_ptr<Menu::Menu>(_scroll.data(), _st.menu),
		_st.scrollPadding);
	_menu = wrap->entity();
	_scroll->setOwnedWidget(std::move(wrap));
	setupMenuWidget();

	// Fill new page.
	fillNew(this);

	// Equalize widths between stashed and new page.
	const auto maxWidth = std::max(
		_menu->width(),
		_stashedContent->menu->width());
	_menu->setForceWidth(maxWidth);
	_stashedContent->menu->setForceWidth(maxWidth);

	handleMenuResize();
}

void PopupMenu::swapStashed(SwitchDirection direction) {
	if (!_stashedContent) {
		return;
	}
	if (_switchState && _switchState->animation.animating()) {
		const auto raw = _switchState.get();
		const auto progress = raw->animation.value(1.);
		raw->animation.stop();

		swapWithStashed();

		std::swap(raw->oldSnapshot, raw->newSnapshot);
		std::swap(raw->fromScrollHeight, raw->toScrollHeight);
		raw->baseY = y();
		raw->positionShift = computePositionShift(raw->toScrollHeight);
		raw->direction = direction;
		startSwitchAnimation(raw, 1. - progress);
		return;
	}
	if (_switchState) {
		_switchState->overlay.destroy();
		_switchState.reset();
		_scroll->show();
		handleMenuResize();
	}

	SendPendingMoveResizeEvents(this);
	const auto oldPixmap = GrabWidget(_menu);
	const auto scrollWidth = _scroll->width();
	const auto oldScrollHeight = _scroll->height();

	// Swap widgets.
	swapWithStashed();
	SendPendingMoveResizeEvents(_menu);

	const auto newPixmap = GrabWidget(_menu);
	const auto newMenuHeight = _menu->height();
	const auto wantedHeight = _st.scrollPadding.top()
		+ newMenuHeight
		+ _st.scrollPadding.bottom();
	const auto newScrollHeight = constrainedScrollHeight(wantedHeight);

	_switchState = std::make_unique<SwitchState>();
	_switchState->direction = direction;
	_switchState->fromScrollHeight = oldScrollHeight;
	_switchState->oldSnapshot = oldPixmap;
	_switchState->newSnapshot = newPixmap;
	_switchState->toScrollHeight = newScrollHeight;

	_scroll->hide();

	// 自绘背景预留透明空间，避免每帧调整系统窗口；原生材质随实际高度变化。
	const auto initialHeight = _systemBackdrop
		? oldScrollHeight
		: std::max(oldScrollHeight, newScrollHeight);
	setFixedSize(
		_padding.left() + scrollWidth + _padding.right(),
		_padding.top() + initialHeight + _padding.bottom());

	_switchState->baseY = y();
	_switchState->positionShift = computePositionShift(newScrollHeight);
	_inner = QRect(
		_padding.left(),
		_padding.top(),
		scrollWidth,
		oldScrollHeight);

	const auto raw = _switchState.get();
	raw->overlay.create(this);
	if (!isWindow()) {
		raw->overlay->setGraphicsEffect(new MenuSurface(_st.radius));
	}
	raw->overlay->setAttribute(Qt::WA_TransparentForMouseEvents);
	raw->overlay->move(_padding.left(), _padding.top());
	raw->overlay->resize(scrollWidth, oldScrollHeight);
	raw->overlay->show();
	if (_roundingOverlay) {
		_roundingOverlay->raise();
	}

	const auto scrollPadding = _st.scrollPadding;

	raw->overlay->paintRequest(
	) | rpl::on_next([=](QRect clip) {
		if (!_switchState || _switchState.get() != raw) {
			return;
		}
		if (!raw->overlay->width() || !raw->overlay->height()) {
			return;
		}
		auto p = QPainter(raw->overlay.data());
		p.fillRect(raw->overlay->rect(), contentBackground());

		const auto progress = raw->animation.value(1.);
		const auto dir = (raw->direction == SwitchDirection::LeftToRight)
			? 1
			: -1;
		const auto shift = anim::interpolate(0, scrollWidth, progress);

		p.drawPixmap(
			scrollPadding.left() - dir * shift,
			scrollPadding.top(),
			raw->oldSnapshot);
		p.drawPixmap(
			scrollPadding.left() + dir * (scrollWidth - shift),
			scrollPadding.top(),
			raw->newSnapshot);
	}, raw->overlay->lifetime());

	startSwitchAnimation(raw, 0.);
}

void PopupMenu::startSwitchAnimation(
		not_null<SwitchState*> raw,
		float64 from) {
	const auto scrollWidth = raw->overlay->width();
	raw->animation.start([=] {
		if (!_switchState || _switchState.get() != raw) {
			return;
		}
		const auto progress = raw->animation.value(1.);
		const auto h = anim::interpolate(
			raw->fromScrollHeight,
			raw->toScrollHeight,
			progress);
		if (_systemBackdrop) {
			setFixedSize(
				width(),
				_padding.top() + h + _padding.bottom());
		}

		if (raw->positionShift) {
			const auto shift = anim::interpolate(
				0,
				raw->positionShift,
				progress);
			move(x(), raw->baseY + shift);
		}

		raw->overlay->resize(scrollWidth, h);
		raw->overlay->update();

		_inner = QRect(
			_padding.left(),
			_padding.top(),
			scrollWidth,
			h);
		update();

		if (!raw->animation.animating()) {
			PostponeCall(this, [=] {
				if (_switchState.get() == raw) {
					finishSwitchAnimation();
				}
			});
		}
	}, from, 1., _st.showDuration * (1. - from), anim::sineInOut);
}

bool PopupMenu::hasStashedContent() const {
	return _stashedContent != nullptr;
}

int PopupMenu::computePositionShift(int targetScrollHeight) const {
	const auto screen = QGuiApplication::screenAt(mapToGlobal(rect().center()));
	if (isWindow() && !screen) {
		return 0;
	}
	const auto r = isWindow()
		? screen->availableGeometry()
		: parentWidget()->contentsRect();
	const auto targetH = _padding.top()
		+ targetScrollHeight
		+ _padding.bottom();
	auto targetY = y();
	if (targetY + targetH - _margins.bottom() > r.y() + r.height()) {
		targetY = r.y() + r.height() + _margins.bottom() - targetH;
	}
	if (targetY + _margins.top() < r.y()) {
		targetY = r.y() - _margins.top();
	}
	return targetY - y();
}

RpWidget *PopupMenu::accessibilityParent() const {
	return dynamic_cast<RpWidget*>(_owner ? _owner.data() : parentWidget());
}

PopupMenu::~PopupMenu() {
	_stashedContent.reset();
	for (const auto &[action, submenu] : base::take(_submenus)) {
		delete submenu;
	}
	if (const auto parent = parentWidget()) {
		const auto focused = QApplication::focusWidget();
		if (isWindow() && _reactivateParent
			&& focused != nullptr
			&& Ui::InFocusChain(parent->window())) {
			ActivateWindowDelayed(parent);
		}
	}
	if (_keepingDelayedActivationPaused) {
		KeepDelayedActivationPaused(false);
	}
	if (_destroyedCallback) {
		_destroyedCallback();
	}
}

QPoint ContextMenuPosition(
		not_null<QWidget*> anchor,
		not_null<QContextMenuEvent*> e) {
	return ContextMenuPosition(anchor, e, anchor->rect());
}

QPoint ContextMenuPosition(
		not_null<QWidget*> anchor,
		not_null<QContextMenuEvent*> e,
		QRect rect) {
	return (e->reason() == QContextMenuEvent::Keyboard)
		? anchor->mapToGlobal(rect.center())
		: QCursor::pos();
}

} // namespace Ui
