#pragma once

#include "ui/widgets/checkbox.h"
#include "ui/wrap/vertical_layout.h"
#include "crl/crl_on_main.h"

namespace Ui {

// 普通选项按完整点击区域排列，保留原控件的选择与键盘行为。
class OptionList final : public VerticalLayout {
public:
	explicit OptionList(
		QWidget *parent,
		const style::margins &padding = st::defaultOptionListPadding)
	: VerticalLayout(parent)
	, _padding(padding) {
	}

	template <
		typename Widget,
		typename = std::enable_if_t<
			std::is_base_of_v<Checkbox, Widget>>>
	Widget *addOption(object_ptr<Widget> &&option) {
		const auto margins = option->getMargins();
		option->setAllowTextLines(0);
		const auto added = VerticalLayout::add(
			std::move(option),
			QMargins(
				_padding.left(),
				std::max(_padding.top(), margins.top()),
				_padding.right(),
				std::max(_padding.bottom(), margins.bottom())),
			style::al_justify);
		added->setFullRectHitTest();
		added->widthValue() | rpl::skip(1) | rpl::on_next([=] {
			const auto available = widthNoMargins()
				- _padding.left() - _padding.right();
			if (available <= 0 || added->widthNoMargins() == available
				|| _reflowPending) {
				return;
			}
			// 文案更新可能重新加上控件边距，下一轮恢复列表分配的宽度。
			_reflowPending = true;
			crl::on_main(this, [=] {
				_reflowPending = false;
				resizeToWidth(widthNoMargins());
			});
		}, lifetime());
		return added;
	}

private:
	using VerticalLayout::add;
	using VerticalLayout::insert;

	const style::margins _padding;
	bool _reflowPending = false;
};

} // namespace Ui
