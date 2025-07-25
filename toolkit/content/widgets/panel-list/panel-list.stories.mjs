/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

import "./panel-list.js";
import { html, ifDefined } from "../vendor/lit.all.mjs";

let accesskeyOptions = ["n", "w", "e", "c", "b"];

export default {
  title: "UI Widgets/Panel List",
  component: "panel-list",
  argTypes: {
    accesskeys: {
      if: { arg: "showAccesskeys", truthy: true },
    },
  },
  parameters: {
    status: "stable",
    actions: {
      handles: ["showing", "shown", "hidden", "click"],
    },
    fluent: `
panel-list-item-one = Item One
panel-list-item-two = Item Two
panel-list-item-three = Item Three
panel-list-disabled = Disabled
panel-list-checked = Checked
panel-list-badged = Badged, look at me
panel-list-passwords = Passwords
panel-list-settings = Settings
submenu-item-one = Submenu Item One
submenu-item-two = Submenu Item Two
submenu-item-three = Submenu Item Three
    `,
  },
};

const Template = ({
  isOpen,
  items,
  wideAnchor,
  hasSubMenu,
  showAccesskeys,
  accesskeys,
}) => html`
  <style>
    panel-item[icon="passwords"]::part(button) {
      background-image: url("chrome://browser/skin/login.svg");
    }
    panel-item[icon="settings"]::part(button) {
      background-image: url("chrome://global/skin/icons/settings.svg");
    }
    moz-button {
      position: absolute;
    }
    moz-button::part(button) {
      background-image: url("chrome://global/skin/icons/more.svg");
    }
    moz-button[wide] {
      width: 400px !important;
    }
    .end {
      inset-inline-end: 30px;
    }

    .bottom {
      inset-block-end: 30px;
    }
  </style>
  ${isOpen
    ? ""
    : html`
        <moz-button
          type="icon ghost"
          menuid="panel-list"
          ?wide=${wideAnchor}
        ></moz-button>
        <moz-button
          type="icon ghost"
          class="end"
          menuid="panel-list"
          ?wide=${wideAnchor}
        ></moz-button>
        <moz-button
          type="icon ghost"
          class="bottom"
          menuid="panel-list"
          ?wide=${wideAnchor}
        ></moz-button>
        <moz-button
          type="icon ghost"
          class="bottom end"
          menuid="panel-list"
          ?wide=${wideAnchor}
        ></moz-button>
      `}
  <panel-list
    id="panel-list"
    ?stay-open=${isOpen}
    ?open=${isOpen}
    ?min-width-from-anchor=${wideAnchor}
  >
    ${items.map((item, index) => {
      // Always showing submenu on the first item for simplicity.
      let showSubMenu = hasSubMenu && index == 0;
      let subMenuId = showSubMenu ? "example-sub-menu" : undefined;
      return item == "<hr>"
        ? html` <hr /> `
        : html`
            <panel-item
              icon=${item.icon ?? ""}
              ?checked=${item.checked}
              ?disabled=${item.disabled}
              type=${ifDefined(item.checked ? "checkbox" : undefined)}
              ?badged=${item.badged}
              data-l10n-id=${item.l10nId ?? item}
              submenu=${ifDefined(subMenuId)}
              accesskey=${ifDefined(showAccesskeys ? accesskeys[index] : "")}
            >
              ${showSubMenu ? subMenuTemplate() : ""}
            </panel-item>
          `;
    })}
  </panel-list>
`;

const subMenuTemplate = () => html`
  <panel-list slot="submenu" id="example-sub-menu">
    <panel-item data-l10n-id="submenu-item-one"></panel-item>
    <panel-item data-l10n-id="submenu-item-two"></panel-item>
    <panel-item data-l10n-id="submenu-item-three"></panel-item>
  </panel-list>
`;

export const Simple = Template.bind({});
Simple.args = {
  isOpen: false,
  wideAnchor: false,
  items: [
    "panel-list-item-one",
    { l10nId: "panel-list-item-two" },
    "panel-list-item-three",
    "<hr>",
    { l10nId: "panel-list-disabled", disabled: true },
    { l10nId: "panel-list-checked", checked: true },
    { l10nId: "panel-list-badged", badged: true, icon: "settings" },
  ],
  showAccesskeys: false,
};

export const Icons = Template.bind({});
Icons.args = {
  isOpen: false,
  wideAnchor: false,
  items: [
    { l10nId: "panel-list-passwords", icon: "passwords" },
    { l10nId: "panel-list-settings", icon: "settings" },
  ],
};

export const Open = Template.bind({});
Open.args = {
  ...Simple.args,
  wideAnchor: false,
  isOpen: true,
};

export const Wide = Template.bind({});
Wide.args = {
  ...Simple.args,
  wideAnchor: true,
};

export const SubMenu = Template.bind({});
SubMenu.args = {
  ...Simple.args,
  hasSubMenu: true,
};

export const WithAccesskeys = Template.bind({});
WithAccesskeys.args = {
  ...Simple.args,
  showAccesskeys: true,
  accesskeys: accesskeyOptions,
};
