/* Cyber Lanlan mobile web application.
 *
 * Plain JavaScript, no build step and no external requests: every URL is
 * relative to the page that served it. The form keeps its client_request_id
 * across retries so a lost response cannot create a second record.
 */
(function () {
  "use strict";

  var UNKNOWN = "未填写";

  var CATEGORY_LABELS = {
    meal: "吃饭",
    water: "喝水",
    care: "护理",
    cleaning: "清洁",
    walk: "散步",
    other: "其他"
  };

  var SUBITEM_LABELS = {
    bath: "洗澡",
    grooming: "梳毛护理",
    teeth: "刷牙",
    comb: "梳子",
    other: "其他",
    ear: "耳朵",
    paw: "爪子",
    pad: "肉垫",
    litter: "猫砂"
  };

  var CATEGORY_RULES = {
    meal: { subitems: [], units: ["g", "ml", "scoop", "cup", "piece", "bag"], amount: true, duration: false, custom: false },
    water: { subitems: [], units: ["ml", "bowl"], amount: true, duration: false, custom: false },
    care: { subitems: ["bath", "grooming", "teeth", "comb", "other"], units: [], amount: false, duration: false, customWhen: "other" },
    cleaning: { subitems: ["ear", "paw", "pad", "litter", "other"], units: [], amount: false, duration: false, customWhen: "other", customOptional: true },
    walk: { subitems: [], units: [], amount: false, duration: true, custom: false },
    other: { subitems: [], units: [], amount: false, duration: false, custom: true }
  };

  var CATEGORY_ORDER = ["meal", "water", "care", "cleaning", "walk", "other"];

  var state = {
    user: null,
    family: null,
    members: [],
    csrf: null,
    formId: null,
    formDirty: false,
    recordCursor: null,
    recordFilters: { from: "", to: "", category: "" },
    editing: null,
    currentDetail: null,
    step: 0,
    devices: []
  };

  var nodes = {};

  function byId(id) {
    return document.getElementById(id);
  }

  function cacheNodes() {
    nodes.netStatus = byId("net-status");
    nodes.tabbar = byId("tabbar");
    nodes.loginView = byId("view-login");
    nodes.loginForm = byId("login-form");
    nodes.loginError = byId("login-error");
    nodes.loginSubmit = byId("login-submit");
    nodes.overviewServerTime = byId("overview-server-time");
    nodes.summaryCounts = byId("summary-counts");
    nodes.summaryLatest = byId("summary-latest");
    nodes.deviceSync = byId("device-sync");
    nodes.overviewRecord = byId("overview-record");
    nodes.recordForm = byId("record-form");
    nodes.recordTitle = byId("record-title");
    nodes.recordCategory = byId("record-category");
    nodes.recordSubitem = byId("record-subitem");
    nodes.recordSubitemField = byId("subitem-field");
    nodes.recordCustomName = byId("record-custom-name");
    nodes.recordCustomNameField = byId("custom-name-field");
    nodes.recordTime = byId("record-time");
    nodes.recordEstimated = byId("record-estimated");
    nodes.recordPerformer = byId("record-performer");
    nodes.recordAmount = byId("record-amount");
    nodes.recordUnit = byId("record-unit");
    nodes.amountField = byId("amount-field");
    nodes.recordDuration = byId("record-duration");
    nodes.durationField = byId("duration-field");
    nodes.recordNote = byId("record-note");
    nodes.recordFeedback = byId("record-feedback");
    nodes.recordSubmit = byId("record-submit");
    nodes.recordRetry = byId("record-retry");
    nodes.recordConflict = byId("record-conflict");
    nodes.filterFrom = byId("filter-from");
    nodes.filterTo = byId("filter-to");
    nodes.filterCategory = byId("filter-category");
    nodes.filterApply = byId("filter-apply");
    nodes.recordList = byId("record-list");
    nodes.recordMore = byId("record-more");
    nodes.recordsEmpty = byId("records-empty");
    nodes.detailBody = byId("detail-body");
    nodes.detailRevisions = byId("detail-revisions");
    nodes.detailEdit = byId("detail-edit");
    nodes.detailRevoke = byId("detail-revoke");
    nodes.detailConflict = byId("detail-conflict");
    nodes.detailFeedback = byId("detail-feedback");
    nodes.reminderList = byId("reminder-list");
    nodes.remindersFeedback = byId("reminders-feedback");
    nodes.profileForm = byId("profile-form");
    nodes.profileName = byId("profile-name");
    nodes.profileBirthday = byId("profile-birthday");
    nodes.profileBreed = byId("profile-breed");
    nodes.profileWeight = byId("profile-weight");
    nodes.profileNotes = byId("profile-notes");
    nodes.profileFeedback = byId("profile-feedback");
    nodes.passwordForm = byId("password-form");
    nodes.passwordCurrent = byId("password-current");
    nodes.passwordNew = byId("password-new");
    nodes.passwordFeedback = byId("password-feedback");
    nodes.caregiverList = byId("caregiver-list");
    nodes.deviceList = byId("device-list");
    nodes.deviceLabel = byId("device-label");
    nodes.deviceCreate = byId("device-create");
    nodes.deviceToken = byId("device-token");
    nodes.deviceFeedback = byId("device-feedback");
    nodes.statusList = byId("status-list");
    nodes.logoutButton = byId("logout-button");
  }

  /* ------------------------------------------------------------------ */
  /* Formatting helpers                                                  */
  /* ------------------------------------------------------------------ */

  function pad(value) {
    return (value < 10 ? "0" : "") + value;
  }

  function formatNumber(value, decimals) {
    if (typeof value !== "number" || !isFinite(value)) {
      return UNKNOWN;
    }
    var rounded = Math.round(value * Math.pow(10, decimals)) / Math.pow(10, decimals);
    return String(rounded);
  }

  function formatAmount(value, unit) {
    if (value === null || value === undefined || typeof value !== "number" || !isFinite(value)) {
      return UNKNOWN;
    }
    var text = formatNumber(value, 2);
    return unit ? text + " " + String(unit) : text;
  }

  function parseUtc(text) {
    if (typeof text !== "string" || !text) {
      return null;
    }
    var match = text.match(/^(\d{4})-(\d{2})-(\d{2})T(\d{2}):(\d{2}):(\d{2})Z?$/);
    if (!match) {
      return null;
    }
    return Date.UTC(
      Number(match[1]),
      Number(match[2]) - 1,
      Number(match[3]),
      Number(match[4]),
      Number(match[5]),
      Number(match[6])
    );
  }

  function formatLocal(text) {
    var millis = parseUtc(text);
    if (millis === null) {
      return UNKNOWN;
    }
    var date = new Date(millis);
    return (
      date.getFullYear() + "-" + pad(date.getMonth() + 1) + "-" + pad(date.getDate()) +
      " " + pad(date.getHours()) + ":" + pad(date.getMinutes())
    );
  }

  function formatLocalShort(text) {
    var millis = parseUtc(text);
    if (millis === null) {
      return UNKNOWN;
    }
    var date = new Date(millis);
    return (
      date.getFullYear() + "-" + pad(date.getMonth() + 1) + "-" + pad(date.getDate()) +
      " " + pad(date.getHours()) + ":" + pad(date.getMinutes())
    );
  }

  function datetimeLocalValue(date) {
    return (
      date.getFullYear() + "-" + pad(date.getMonth() + 1) + "-" + pad(date.getDate()) +
      "T" + pad(date.getHours()) + ":" + pad(date.getMinutes())
    );
  }

  function toUtcIso(localValue) {
    if (typeof localValue !== "string" || localValue.length < 16) {
      return null;
    }
    var parts = localValue.match(/^(\d{4})-(\d{2})-(\d{2})T(\d{2}):(\d{2})/);
    if (!parts) {
      return null;
    }
    var date = new Date(
      Number(parts[1]), Number(parts[2]) - 1, Number(parts[3]),
      Number(parts[4]), Number(parts[5]), 0
    );
    if (date.getFullYear() !== Number(parts[1]) || date.getMonth() !== Number(parts[2]) - 1 ||
        date.getDate() !== Number(parts[3]) || date.getHours() !== Number(parts[4]) ||
        date.getMinutes() !== Number(parts[5])) return null;
    return date.toISOString().replace(/\.\d{3}Z$/, "Z");
  }

  function text(value) {
    if (value === null || value === undefined || value === "") {
      return UNKNOWN;
    }
    return String(value);
  }

  function categoryLabel(category) {
    return CATEGORY_LABELS[category] || text(category);
  }

  function subitemLabel(key) {
    if (!key) {
      return "";
    }
    return SUBITEM_LABELS[key] || String(key);
  }

  function recordTitle(record) {
    var label = categoryLabel(record.category);
    if (record.custom_name) {
      return label + " · " + record.custom_name;
    }
    if (record.subitem) {
      return label + " · " + subitemLabel(record.subitem);
    }
    return label;
  }

  function memberName(id) {
    for (var index = 0; index < state.members.length; index += 1) {
      if (state.members[index].id === id) {
        return state.members[index].display_name || state.members[index].username;
      }
    }
    return text(id);
  }

  function describeRecord(record) {
    var parts = ["数量 " + formatAmount(record.amount_value, record.amount_unit)];
    if (record.duration_minutes !== null && record.duration_minutes !== undefined) {
      parts.push(String(record.duration_minutes) + " 分钟");
    }
    return parts.join(" · ");
  }

  function node(tag, className, content) {
    var element = document.createElement(tag);
    if (className) {
      element.className = className;
    }
    if (content !== undefined && content !== null) {
      element.textContent = String(content);
    }
    return element;
  }

  function clear(element) {
    while (element.firstChild) {
      element.removeChild(element.firstChild);
    }
  }

  function showError(element, message) {
    if (!element) {
      return;
    }
    element.className = "error";
    if (!message) {
      element.hidden = true;
      element.textContent = "";
      return;
    }
    element.hidden = false;
    element.textContent = String(message);
  }

  function showNotice(element, message) {
    if (!element) {
      return;
    }
    element.className = "notice";
    element.hidden = !message;
    element.textContent = message ? String(message) : "";
  }

  function errorMessage(payload, fallback) {
    if (payload && payload.error && payload.error.message) {
      return String(payload.error.message);
    }
    if (payload && payload.message) {
      return String(payload.message);
    }
    return fallback;
  }

  /* ------------------------------------------------------------------ */
  /* API client                                                          */
  /* ------------------------------------------------------------------ */

  var pendingTimer = null;

  function setNetState(stateName, label) {
    if (!nodes.netStatus) {
      return;
    }
    nodes.netStatus.setAttribute("data-state", stateName);
    nodes.netStatus.textContent = label;
  }

  function startPending() {
    setNetState("slow", "同步中…");
    if (pendingTimer) {
      clearTimeout(pendingTimer);
    }
    pendingTimer = setTimeout(function () {
      setNetState("slow", "网络较慢…");
    }, 1500);
  }

  function stopPending(ok) {
    if (pendingTimer) {
      clearTimeout(pendingTimer);
      pendingTimer = null;
    }
    setNetState(ok ? "online" : "offline", ok ? "已连接" : "离线");
  }

  function api(method, path, body) {
    var options = {
      method: method,
      headers: { "Accept": "application/json" },
      credentials: "same-origin"
    };
    var isWrite = method !== "GET" && method !== "HEAD";
    if (body !== undefined && body !== null) {
      options.headers["Content-Type"] = "application/json";
      options.body = JSON.stringify(body);
    }
    if (isWrite && state.csrf) {
      options.headers["X-Lanlan-CSRF"] = state.csrf;
    }
    startPending();
    return fetch(path, options)
      .then(function (response) {
        return response.text().then(function (raw) {
          var payload = null;
          if (raw) {
            try {
              payload = JSON.parse(raw);
            } catch (error) {
              payload = null;
            }
          }
          stopPending(response.status < 500);
          return { status: response.status, ok: response.ok, data: payload };
        });
      })
      .catch(function (error) {
        stopPending(false);
        return { status: 0, ok: false, data: null, networkError: String(error) };
      });
  }

  /* ------------------------------------------------------------------ */
  /* Routing                                                             */
  /* ------------------------------------------------------------------ */

  var VIEWS = {
    login: "view-login",
    overview: "view-overview",
    record: "view-record",
    records: "view-records",
    detail: "view-detail",
    reminders: "view-reminders",
    profile: "view-profile",
    account: "view-account"
  };

  function parseHash() {
    var raw = window.location.hash.replace(/^#\/?/, "");
    var parts = raw.split("/");
    var name = parts[0] || "overview";
    return { name: name, arg: parts[1] || "" };
  }

  function showView(name) {
    var keys = Object.keys(VIEWS);
    for (var index = 0; index < keys.length; index += 1) {
      var element = byId(VIEWS[keys[index]]);
      if (element) {
        element.hidden = true;
      }
    }
    var target = byId(VIEWS[name] || VIEWS.overview);
    if (target) {
      target.hidden = false;
    }
    var links = nodes.tabbar ? nodes.tabbar.querySelectorAll("a") : [];
    for (var linkIndex = 0; linkIndex < links.length; linkIndex += 1) {
      if (links[linkIndex].getAttribute("data-tab") === name) {
        links[linkIndex].className = "active";
      } else {
        links[linkIndex].className = "";
      }
    }
    window.scrollTo(0, 0);
  }

  function render(route) {
    if (!state.user) {
      showView("login");
      if (nodes.tabbar) {
        nodes.tabbar.hidden = true;
      }
      return;
    }
    if (nodes.tabbar) {
      nodes.tabbar.hidden = false;
    }
    if (route.name === "login") {
      window.location.hash = "#/overview";
      return;
    }
    showView(route.name in VIEWS ? route.name : "overview");
    if (route.name === "overview") {
      loadOverview();
    } else if (route.name === "record") {
      prepareRecordForm(route.arg);
    } else if (route.name === "records") {
      loadRecordList(true);
    } else if (route.name === "detail") {
      loadDetail(route.arg);
    } else if (route.name === "reminders") {
      loadReminders();
    } else if (route.name === "profile") {
      loadProfile();
    } else if (route.name === "account") {
      loadAccount();
    }
  }

  /* ------------------------------------------------------------------ */
  /* Login and session                                                   */
  /* ------------------------------------------------------------------ */

  function applySession(payload) {
    state.user = payload.user || null;
    state.family = payload.family || null;
    state.members = payload.members || [];
    if (payload.csrf_token) {
      state.csrf = payload.csrf_token;
    }
  }

  function handleLogin(event) {
    event.preventDefault();
    showError(nodes.loginError, "");
    var username = byId("login-username").value.trim();
    var password = byId("login-password").value;
    if (!username || !password) {
      showError(nodes.loginError, "请输入用户名和密码。");
      return;
    }
    nodes.loginSubmit.disabled = true;
    api("POST", "api/v1/auth/login", { username: username, password: password }).then(function (result) {
      nodes.loginSubmit.disabled = false;
      if (result.ok && result.data) {
        byId("login-password").value = "";
        applySession(result.data);
        window.location.hash = "#/overview";
        render(parseHash());
        return;
      }
      if (result.status === 429) {
        showError(nodes.loginError, errorMessage(result.data, "尝试次数过多，请稍后再试。"));
      } else if (result.status === 0) {
        showError(nodes.loginError, "无法连接服务器，请检查网络后重试。");
      } else {
        showError(nodes.loginError, errorMessage(result.data, "用户名或密码不正确。"));
      }
    });
  }

  function handleLogout() {
    api("POST", "api/v1/auth/logout", {}).then(function () {
      state.user = null;
      state.family = null;
      state.members = [];
      state.csrf = null;
      window.location.hash = "#/login";
      render(parseHash());
    });
  }

  function ensureSession() {
    return api("GET", "api/v1/me").then(function (result) {
      if (result.ok && result.data) {
        applySession(result.data);
        return true;
      }
      state.user = null;
      state.csrf = null;
      return false;
    });
  }

  function guard(result) {
    if (result.status === 401) {
      state.user = null;
      state.csrf = null;
      window.location.hash = "#/login";
      render(parseHash());
      showError(nodes.loginError, "登录已过期，请重新登录。");
      return false;
    }
    if (result.status === 403) {
      return false;
    }
    return true;
  }

  /* ------------------------------------------------------------------ */
  /* Overview                                                            */
  /* ------------------------------------------------------------------ */

  function loadOverview() {
    api("GET", "api/v1/summary/today").then(function (result) {
      if (!guard(result)) {
        return;
      }
      if (!result.ok || !result.data) {
        if (nodes.overviewServerTime) {
          nodes.overviewServerTime.textContent = "无法读取今日汇总，稍后重试。";
        }
        return;
      }
      var data = result.data;
      nodes.overviewServerTime.textContent =
        (data.date ? data.date + " · " : "") +
        "服务器时间 " + formatLocalShort(data.server_time) +
        "（家庭时区 " + text(data.timezone) + "，UTC" +
        (data.utc_offset_minutes >= 0 ? "+" : "") + String(data.utc_offset_minutes / 60) + "）";
      clear(nodes.summaryCounts);
      var counts = data.counts || {};
      for (var index = 0; index < CATEGORY_ORDER.length; index += 1) {
        var key = CATEGORY_ORDER[index];
        var cell = node("div", "count");
        cell.appendChild(node("div", "n", String(counts[key] || 0)));
        cell.appendChild(node("div", "k", categoryLabel(key)));
        nodes.summaryCounts.appendChild(cell);
      }
      clear(nodes.summaryLatest);
      var latest = data.latest || {};
      var keys = ["meal", "water", "walk"];
      for (var latestIndex = 0; latestIndex < keys.length; latestIndex += 1) {
        var item = latest[keys[latestIndex]];
        var row = document.createElement("li");
        row.appendChild(node("span", "", "最近一次" + categoryLabel(keys[latestIndex])));
        row.appendChild(node("span", "", item ? formatLocalShort(item.at) : UNKNOWN));
        nodes.summaryLatest.appendChild(row);
      }
    });

    api("GET", "api/v1/status").then(function (result) {
      if (!result.ok || !result.data) {
        nodes.deviceSync.textContent = "设备同步时间" + UNKNOWN + "。";
        return;
      }
      var lastSync = result.data.last_device_sync_at;
      nodes.deviceSync.textContent = lastSync
        ? "最近一次设备同步：" + formatLocalShort(lastSync)
        : "还没有设备同步过。";
    });
  }

  /* ------------------------------------------------------------------ */
  /* Record form                                                         */
  /* ------------------------------------------------------------------ */

  function fillSelect(select, options, selected) {
    clear(select);
    for (var index = 0; index < options.length; index += 1) {
      var option = document.createElement("option");
      option.value = options[index].value;
      option.textContent = options[index].label;
      if (options[index].value === selected) {
        option.selected = true;
      }
      select.appendChild(option);
    }
  }

  function fillCategories() {
    var options = [];
    for (var index = 0; index < CATEGORY_ORDER.length; index += 1) {
      var key = CATEGORY_ORDER[index];
      options.push({ value: key, label: CATEGORY_LABELS[key] });
    }
    fillSelect(nodes.recordCategory, options, nodes.recordCategory.value || "meal");
    fillSelect(nodes.filterCategory, [{ value: "", label: "全部" }].concat(options), nodes.filterCategory.value || "");
  }

  function fillMembers() {
    var options = [];
    for (var index = 0; index < state.members.length; index += 1) {
      options.push({
        value: state.members[index].id,
        label: state.members[index].display_name || state.members[index].username
      });
    }
    var selected = nodes.recordPerformer.value || (state.user ? state.user.id : "");
    fillSelect(nodes.recordPerformer, options, selected);
    if (!nodes.recordPerformer.value && state.user) {
      nodes.recordPerformer.value = state.user.id;
    }
  }

  function syncCategoryFields() {
    var category = nodes.recordCategory.value;
    var rules = CATEGORY_RULES[category] || { subitems: [], units: [] };
    var needsSubitem = rules.subitems && rules.subitems.length > 0;
    nodes.recordSubitemField.hidden = !needsSubitem;
    if (needsSubitem) {
      var keep = nodes.recordSubitem.value;
      var options = [];
      for (var index = 0; index < rules.subitems.length; index += 1) {
        var key = rules.subitems[index];
        options.push({ value: key, label: subitemLabel(key) });
      }
      fillSelect(nodes.recordSubitem, options, keep);
      if (!nodes.recordSubitem.value) {
        nodes.recordSubitem.value = rules.subitems[0];
      }
    }
    var needsCustom =
      rules.custom === true ||
      (rules.customWhen && nodes.recordSubitem.value === rules.customWhen);
    nodes.recordCustomNameField.hidden = !needsCustom;
    nodes.amountField.hidden = !rules.amount;
    if (rules.amount) {
      var unitOptions = [];
      for (var unitIndex = 0; unitIndex < rules.units.length; unitIndex += 1) {
        unitOptions.push({ value: rules.units[unitIndex], label: rules.units[unitIndex] });
      }
      var currentUnit = nodes.recordUnit.value;
      fillSelect(nodes.recordUnit, unitOptions, currentUnit);
      if (!nodes.recordUnit.value && unitOptions.length) {
        nodes.recordUnit.value = unitOptions[0].value;
      }
    }
    nodes.durationField.hidden = !rules.duration;
  }

  function resetRecordForm() {
    state.editing = null;
    state.formId = newRequestId();
    state.formDirty = false;
    state.step = 0;
    nodes.recordTitle.textContent = "记一笔";
    nodes.recordForm.reset();
    nodes.recordCategory.value = "meal";
    nodes.recordEstimated.checked = false;
    nodes.recordTime.value = datetimeLocalValue(new Date());
    showError(nodes.recordFeedback, "");
    showError(nodes.recordConflict, "");
    nodes.recordRetry.hidden = true;
    nodes.recordSubmit.disabled = false;
    nodes.recordSubmit.textContent = "保存到服务器";
    fillMembers();
    syncCategoryFields();
  }

  function createRequestId() {
    if (window.crypto && typeof window.crypto.randomUUID === "function") {
      return window.crypto.randomUUID();
    }
    var bytes = new Uint8Array(16);
    if (window.crypto && window.crypto.getRandomValues) {
      window.crypto.getRandomValues(bytes);
    } else {
      for (var index = 0; index < bytes.length; index += 1) {
        bytes[index] = Math.floor(Math.random() * 256);
      }
    }
    bytes[6] = (bytes[6] & 0x0f) | 0x40;
    bytes[8] = (bytes[8] & 0x3f) | 0x80;
    var hex = [];
    for (var hexIndex = 0; hexIndex < bytes.length; hexIndex += 1) {
      hex.push((bytes[hexIndex] + 0x100).toString(16).slice(1));
    }
    return (
      hex[0] + hex[1] + hex[2] + hex[3] + "-" + hex[4] + hex[5] + "-" + hex[6] + hex[7] + "-" +
      hex[8] + hex[9] + "-" + hex[10] + hex[11] + hex[12] + hex[13] + hex[14] + hex[15]
    );
  }

  function newRequestId() {
    return createRequestId();
  }

  function prepareRecordForm(recordId) {
    fillMembers();
    if (recordId) {
      api("GET", "api/v1/records/" + encodeURIComponent(recordId)).then(function (result) {
        if (!result.ok || !result.data) {
          showError(nodes.recordFeedback, errorMessage(result.data, "无法读取这条记录。"));
          return;
        }
        fillRecordForm(result.data.record);
      });
      return;
    }
    if (!state.formId || state.formDirty || state.editing) {
      resetRecordForm();
    }
  }

  function fillRecordForm(record) {
    state.editing = record;
    state.formId = null;
    state.step = 0;
    nodes.recordTitle.textContent = "修改记录";
    nodes.recordCategory.value = record.category;
    syncCategoryFields();
    if (record.subitem) {
      nodes.recordSubitem.value = record.subitem;
      syncCategoryFields();
    }
    nodes.recordCustomName.value = record.custom_name || "";
    var millis = parseUtc(record.occurred_at);
    nodes.recordTime.value = millis === null ? datetimeLocalValue(new Date()) : datetimeLocalValue(new Date(millis));
    nodes.recordEstimated.checked = record.time_confidence === "estimated";
    fillMembers();
    nodes.recordPerformer.value = record.performed_by;
    nodes.recordAmount.value = record.amount_value === null || record.amount_value === undefined
      ? "" : String(record.amount_value);
    if (record.amount_unit) {
      syncCategoryFields();
      nodes.recordUnit.value = record.amount_unit;
    }
    nodes.recordDuration.value =
      record.duration_minutes === null || record.duration_minutes === undefined
        ? ""
        : String(record.duration_minutes);
    nodes.recordNote.value = record.note || "";
    showError(nodes.recordFeedback, "");
    showError(nodes.recordConflict, "");
    nodes.recordRetry.hidden = true;
    nodes.recordSubmit.disabled = false;
    nodes.recordSubmit.textContent = "保存修改";
  }

  function collectRecordBody(includeRequestId) {
    var category = nodes.recordCategory.value;
    var rules = CATEGORY_RULES[category] || {};
    var body = {
      category: category,
      occurred_at: toUtcIso(nodes.recordTime.value),
      time_confidence: nodes.recordEstimated.checked ? "estimated" : "trusted",
      performed_by: nodes.recordPerformer.value
    };
    if (rules.subitems && rules.subitems.length) {
      body.subitem = nodes.recordSubitem.value;
    }
    var needsCustom =
      rules.custom === true || (rules.customWhen && nodes.recordSubitem.value === rules.customWhen);
    if (needsCustom) {
      body.custom_name = nodes.recordCustomName.value.trim();
    }
    if (rules.amount && nodes.recordAmount.value !== "") {
      var amount = Number(nodes.recordAmount.value);
      body.amount_value = isFinite(amount) ? amount : null;
      body.amount_unit = nodes.recordUnit.value;
    }
    if (rules.duration && nodes.recordDuration.value !== "") {
      body.duration_minutes = parseInt(nodes.recordDuration.value, 10);
    }
    if (nodes.recordNote.value.trim() !== "") {
      body.note = nodes.recordNote.value.trim();
    }
    if (includeRequestId && state.formId) {
      body.client_request_id = state.formId;
    }
    if (state.editing) {
      body.expected_version = state.editing.version;
    }
    return body;
  }

  function handleRecordSubmit(event) {
    event.preventDefault();
    showError(nodes.recordFeedback, "");
    showError(nodes.recordConflict, "");
    var body = collectRecordBody(!state.editing);
    if (!body.occurred_at) {
      showError(nodes.recordFeedback, "请选择发生时间。");
      return;
    }
    if (body.custom_name !== undefined && (body.custom_name.length < 1 || body.custom_name.length > 12)) {
      showError(nodes.recordFeedback, "名称需要 1 到 12 个字。");
      return;
    }
    if (body.amount_value === null) {
      showError(nodes.recordFeedback, "数量需要是数字，或者留空表示未知。");
      return;
    }
    nodes.recordSubmit.disabled = true;
    nodes.recordRetry.hidden = true;

    var path = "api/v1/records";
    var method = "POST";
    if (state.editing) {
      path = "api/v1/records/" + encodeURIComponent(state.editing.id);
      method = "PATCH";
    }
    state.step += 1;
    api(method, path, body).then(function (result) {
      nodes.recordSubmit.disabled = false;
      if (result.status === 409 && result.data && result.data.error &&
          result.data.error.code === "idempotency_conflict") {
        // The first POST succeeded but its response was lost. Keep the edited
        // fields, and let the user explicitly save them as a revision.
        state.editing = result.data.record;
        nodes.recordSubmit.textContent = "保存修改";
        showError(nodes.recordFeedback, "上一次提交已经保存。你后来改动了内容，当前填写已保留，再点保存修改即可更新那笔记录。");
        state.formDirty = true;
        nodes.recordRetry.hidden = true;
        return;
      }
      if (result.status === 409 && result.data && result.data.record) {
        showConflictRecord(result.data.record);
        showError(
          nodes.recordFeedback,
          "服务器上的版本更新（v" + String(result.data.record.version) +
            "），已保留你填写的内容，请刷新后重新提交。"
        );
        state.formDirty = true;
        return;
      }
      if (result.status === 422) {
        showError(nodes.recordFeedback, errorMessage(result.data, "内容不符合规则，已保留填写内容。"));
        state.formDirty = true;
        nodes.recordRetry.hidden = false;
        return;
      }
      if (result.status === 401 || result.status === 403) {
        guard(result);
        showError(nodes.recordFeedback, errorMessage(result.data, "登录状态已失效，请重新登录。"));
        return;
      }
      if (result.status === 0) {
        showError(nodes.recordFeedback, "无法连接服务器，已保留填写内容，可以重试。");
        state.formDirty = true;
        nodes.recordRetry.hidden = false;
        return;
      }
      if (!result.ok || !result.data) {
        showError(nodes.recordFeedback, errorMessage(result.data, "保存失败，已保留填写内容。"));
        state.formDirty = true;
        nodes.recordRetry.hidden = false;
        return;
      }
      var record = result.data.record;
      var replay = result.data.idempotent_replay === true;
      nodes.recordRetry.hidden = true;
      showError(
        nodes.recordFeedback,
        replay ? "这次提交之前已经保存过，服务器返回了同一笔记录。" : ""
      );
      if (state.editing) {
        state.editing = record;
        nodes.recordSubmit.textContent = "保存修改";
        window.location.hash = "#/detail/" + record.id;
        return;
      }
      state.formId = newRequestId();
      resetRecordForm();
      window.location.hash = "#/detail/" + record.id;
    });
  }

  function showConflictRecord(record) {
    nodes.recordConflict.hidden = false;
    nodes.recordConflict.textContent =
      "冲突：服务器当前版本 v" + String(record.version) + "，修改人 " +
      memberName(record.created_by) + "，时间 " + formatLocalShort(record.created_at) + "。";
  }

  /* ------------------------------------------------------------------ */
  /* Record list                                                         */
  /* ------------------------------------------------------------------ */

  function recordEntryElement(record) {
    var item = document.createElement("li");
    var link = document.createElement("a");
    link.className = "entry";
    link.href = "#/detail/" + encodeURIComponent(record.id);
    var head = node("div", "entry-head");
    head.appendChild(node("span", "entry-title", recordTitle(record)));
    var badge = node("span", "badge", record.status === "revoked" ? "已作废" : "有效");
    if (record.status === "revoked") {
      badge.className = "badge revoked";
    }
    head.appendChild(badge);
    link.appendChild(head);
    link.appendChild(node("div", "entry-sub",
      formatLocalShort(record.occurred_at) + " · " + describeRecord(record)));
    link.appendChild(node("div", "entry-meta",
      "记录人 " + memberName(record.created_by) + " · 执行人 " + memberName(record.performed_by) +
      (record.time_confidence === "estimated" ? " · 时间约估" : "")));
    item.appendChild(link);
    return item;
  }

  function loadRecordList(reset) {
    if (reset) {
      state.recordCursor = null;
      clear(nodes.recordList);
    }
    var query = ["limit=20"];
    var filters = state.recordFilters;
    if (filters.from) {
      query.push("from=" + encodeURIComponent(filters.from));
    }
    if (filters.to) {
      query.push("to=" + encodeURIComponent(filters.to));
    }
    if (filters.category) {
      query.push("category=" + encodeURIComponent(filters.category));
    }
    if (state.recordCursor) {
      query.push("cursor=" + encodeURIComponent(state.recordCursor));
    }
    api("GET", "api/v1/records?" + query.join("&")).then(function (result) {
      if (!guard(result)) {
        return;
      }
      if (!result.ok || !result.data) {
        showError(nodes.remindersFeedback, "");
        nodes.recordsEmpty.hidden = false;
        nodes.recordsEmpty.textContent = errorMessage(result.data, "无法读取记录列表。");
        return;
      }
      var records = result.data.records || [];
      for (var index = 0; index < records.length; index += 1) {
        nodes.recordList.appendChild(recordEntryElement(records[index]));
      }
      state.recordCursor = result.data.next_cursor || null;
      nodes.recordMore.hidden = !state.recordCursor;
      nodes.recordsEmpty.hidden = !(reset && records.length === 0);
    });
  }

  function applyFilters() {
    state.recordFilters = {
      from: nodes.filterFrom.value,
      to: nodes.filterTo.value,
      category: nodes.filterCategory.value
    };
    loadRecordList(true);
  }

  /* ------------------------------------------------------------------ */
  /* Detail                                                              */
  /* ------------------------------------------------------------------ */

  function loadDetail(recordId) {
    if (!recordId) {
      window.location.hash = "#/records";
      return;
    }
    showError(nodes.detailConflict, "");
    showError(nodes.detailFeedback, "");
    api("GET", "api/v1/records/" + encodeURIComponent(recordId)).then(function (result) {
      if (!guard(result)) {
        return;
      }
      if (!result.ok || !result.data) {
        showError(nodes.detailFeedback, errorMessage(result.data, "无法读取这条记录。"));
        return;
      }
      state.currentDetail = result.data.record;
      renderDetail(result.data.record, result.data.revisions || []);
    });
  }

  function renderDetail(record, revisions) {
    clear(nodes.detailBody);
    nodes.detailBody.appendChild(node("h2", "", recordTitle(record)));
    var rows = [
      ["发生时间", formatLocalShort(record.occurred_at) + (record.time_confidence === "estimated" ? "（约估）" : "")],
      ["数量", record.amount_value === null || record.amount_value === undefined
        ? UNKNOWN : formatAmount(record.amount_value, record.amount_unit)],
      ["时长", record.duration_minutes === null || record.duration_minutes === undefined
        ? UNKNOWN : String(record.duration_minutes) + " 分钟"],
      ["记录人", memberName(record.created_by)],
      ["执行人", memberName(record.performed_by)],
      ["备注", record.note ? String(record.note) : "无"],
      ["状态", record.status === "revoked" ? "已作废" : "有效"],
      ["版本", "v" + String(record.version)]
    ];
    var list = document.createElement("ul");
    list.className = "plain";
    for (var index = 0; index < rows.length; index += 1) {
      var item = document.createElement("li");
      item.appendChild(node("span", "", rows[index][0]));
      item.appendChild(node("span", "", rows[index][1]));
      list.appendChild(item);
    }
    nodes.detailBody.appendChild(list);

    clear(nodes.detailRevisions);
    for (var revisionIndex = 0; revisionIndex < revisions.length; revisionIndex += 1) {
      var revision = revisions[revisionIndex];
      var entry = document.createElement("li");
      entry.appendChild(node("div", "entry-title",
        "v" + String(revision.version) + " · " + (revision.status === "revoked" ? "作废" : "修改")));
      entry.appendChild(node("div", "entry-meta",
        memberName(revision.created_by) + " · " + formatLocalShort(revision.created_at) +
        (revision.revoke_reason ? " · 原因：" + revision.revoke_reason : "")));
      entry.appendChild(node("div", "entry-sub",
        formatLocalShort(revision.occurred_at) + " · " + describeRecord(revision)));
      nodes.detailRevisions.appendChild(entry);
    }
    nodes.detailEdit.disabled = record.status === "revoked";
    nodes.detailRevoke.disabled = record.status === "revoked";
  }

  function handleDetailEdit() {
    if (!state.currentDetail) {
      return;
    }
    window.location.hash = "#/record/" + state.currentDetail.id;
  }

  function handleDetailRevoke() {
    if (!state.currentDetail) {
      return;
    }
    var record = state.currentDetail;
    var confirmed = window.confirm("确认作废这条记录？作废后仍可在修订历史中查看。");
    if (!confirmed) {
      return;
    }
    api("POST", "api/v1/records/" + encodeURIComponent(record.id) + "/revoke", {
      expected_version: record.version,
      reason: "页面作废"
    }).then(function (result) {
      if (result.status === 409 && result.data && result.data.record) {
        showError(nodes.detailConflict, "");
        nodes.detailConflict.hidden = false;
        nodes.detailConflict.textContent =
          "冲突：服务器当前版本 v" + String(result.data.record.version) +
          "，请刷新后决定。";
        state.currentDetail = result.data.record;
        loadDetail(record.id);
        return;
      }
      if (!result.ok || !result.data) {
        showError(nodes.detailFeedback, errorMessage(result.data, "作废失败，请重试。"));
        return;
      }
      loadDetail(record.id);
    });
  }

  /* ------------------------------------------------------------------ */
  /* Reminders                                                           */
  /* ------------------------------------------------------------------ */

  function reminderLabel(reminder) {
    var label = categoryLabel(reminder.category);
    if (reminder.custom_name) {
      return label + " · " + reminder.custom_name;
    }
    if (reminder.subitem) {
      return label + " · " + subitemLabel(reminder.subitem);
    }
    return label;
  }

  function loadReminders() {
    api("GET", "api/v1/reminders").then(function (result) {
      if (!guard(result)) {
        return;
      }
      if (!result.ok || !result.data) {
        showError(nodes.remindersFeedback, errorMessage(result.data, "无法读取提醒。"));
        return;
      }
      clear(nodes.reminderList);
      var reminders = result.data.reminders || [];
      for (var index = 0; index < reminders.length; index += 1) {
        nodes.reminderList.appendChild(reminderRow(reminders[index]));
      }
    });
  }

  function reminderRow(reminder) {
    var item = document.createElement("li");
    var row = node("div", "reminder-row");
    var grow = node("div", "grow");
    grow.appendChild(node("div", "title", reminderLabel(reminder)));
    var hint = node("div", "entry-meta", "每天 · 家庭时区");
    grow.appendChild(hint);
    row.appendChild(grow);

    var timeInput = document.createElement("input");
    timeInput.type = "time";
    timeInput.value = reminder.time_local || "";
    timeInput.setAttribute("aria-label", reminderLabel(reminder) + " 时间");
    row.appendChild(timeInput);

    var enable = document.createElement("input");
    enable.type = "checkbox";
    enable.checked = reminder.enabled === true;
    enable.setAttribute("aria-label", reminderLabel(reminder) + " 开启");
    row.appendChild(enable);

    var sound = document.createElement("input");
    sound.type = "checkbox";
    sound.checked = false;
    sound.disabled = true;
    sound.setAttribute("aria-label", reminderLabel(reminder) + " 提醒声音（设备端设置）");
    row.appendChild(sound);

    function patch(body) {
      api("PATCH", "api/v1/reminders/" + encodeURIComponent(reminder.id), body).then(function (result) {
        if (result.status === 422) {
          showError(nodes.remindersFeedback, "开启提醒前需要先选择时间。");
          loadReminders();
          return;
        }
        if (!result.ok || !result.data) {
          showError(nodes.remindersFeedback, errorMessage(result.data, "保存提醒失败。"));
          return;
        }
        showError(nodes.remindersFeedback, "");
      });
    }

    enable.addEventListener("change", function () {
      if (enable.checked && !timeInput.value) {
        showError(nodes.remindersFeedback, "请先选择提醒时间，再开启。");
        enable.checked = false;
        timeInput.focus();
        return;
      }
      patch({ enabled: enable.checked, time_local: timeInput.value || null });
    });

    timeInput.addEventListener("change", function () {
      if (!timeInput.value) {
        patch({ enabled: false, time_local: null });
        return;
      }
      patch({ enabled: enable.checked, time_local: timeInput.value });
    });

    item.appendChild(row);
    return item;
  }

  /* ------------------------------------------------------------------ */
  /* Profile                                                             */
  /* ------------------------------------------------------------------ */

  function loadProfile() {
    api("GET", "api/v1/profile").then(function (result) {
      if (!guard(result)) {
        return;
      }
      if (!result.ok || !result.data) {
        showError(nodes.profileFeedback, errorMessage(result.data, "无法读取档案。"));
        return;
      }
      var profile = result.data.profile || {};
      nodes.profileName.value = profile.pet_name || "";
      nodes.profileBirthday.value = profile.birthday || "";
      nodes.profileBreed.value = profile.breed || "";
      nodes.profileWeight.value =
        profile.weight_grams === null || profile.weight_grams === undefined ? "" : String(profile.weight_grams);
      nodes.profileNotes.value = profile.notes || "";
      showError(nodes.profileFeedback, "");
    });
  }

  function handleProfileSubmit(event) {
    event.preventDefault();
    var body = {
      pet_name: nodes.profileName.value.trim() || null,
      birthday: nodes.profileBirthday.value || null,
      breed: nodes.profileBreed.value.trim() || null,
      weight_grams: nodes.profileWeight.value === "" ? null : parseInt(nodes.profileWeight.value, 10),
      notes: nodes.profileNotes.value.trim() || null
    };
    api("PATCH", "api/v1/profile", body).then(function (result) {
      if (!result.ok || !result.data) {
        showError(nodes.profileFeedback, errorMessage(result.data, "保存失败，请重试。"));
        return;
      }
      showNotice(nodes.profileFeedback, "已保存。");
    });
  }

  /* ------------------------------------------------------------------ */
  /* Account                                                             */
  /* ------------------------------------------------------------------ */

  function loadAccount() {
    clear(nodes.caregiverList);
    for (var index = 0; index < state.members.length; index += 1) {
      var item = document.createElement("li");
      item.appendChild(node("span", "", state.members[index].display_name || state.members[index].username));
      item.appendChild(node("span", "", state.members[index].username));
      nodes.caregiverList.appendChild(item);
    }
    loadDevices();
    api("GET", "api/v1/status").then(function (result) {
      clear(nodes.statusList);
      if (!result.ok || !result.data) {
        return;
      }
      var data = result.data;
      var rows = [
        ["服务器时间", formatLocalShort(data.server_time)],
        ["家庭时区", text(data.timezone)],
        ["运行模式", data.storage_mode === "production" ? "production" : "development"],
        ["有效记录", String(data.active_records)],
        ["设备数量", String(data.device_count)],
        ["最近设备同步", data.last_device_sync_at ? formatLocalShort(data.last_device_sync_at) : UNKNOWN]
      ];
      for (var rowIndex = 0; rowIndex < rows.length; rowIndex += 1) {
        var row = document.createElement("li");
        row.appendChild(node("span", "", rows[rowIndex][0]));
        row.appendChild(node("span", "", rows[rowIndex][1]));
        nodes.statusList.appendChild(row);
      }
    });
  }

  function loadDevices() {
    api("GET", "api/v1/devices").then(function (result) {
      clear(nodes.deviceList);
      if (!result.ok || !result.data) {
        return;
      }
      var devices = result.data.devices || [];
      state.devices = devices;
      if (!devices.length) {
        var empty = document.createElement("li");
        empty.appendChild(node("span", "", "还没有设备"));
        empty.appendChild(node("span", "", "—"));
        nodes.deviceList.appendChild(empty);
        return;
      }
      for (var index = 0; index < devices.length; index += 1) {
        (function (device) {
          var item = document.createElement("li");
          var info = node("div", "grow");
          info.appendChild(node("div", "", device.label + (device.revoked ? "（已吊销）" : "")));
          info.appendChild(node("div", "entry-meta",
            "创建 " + formatLocalShort(device.created_at) +
            " · 最近 " + (device.last_seen_at ? formatLocalShort(device.last_seen_at) : UNKNOWN)));
          item.appendChild(info);
          if (!device.revoked) {
            var button = document.createElement("button");
            button.type = "button";
            button.className = "danger";
            button.textContent = "吊销";
            button.addEventListener("click", function () {
              if (!window.confirm("确认吊销这个设备凭据？")) {
                return;
              }
              api("POST", "api/v1/devices/" + encodeURIComponent(device.id) + "/revoke", {}).then(function (revokeResult) {
                if (!revokeResult.ok) {
                  showError(nodes.deviceFeedback, errorMessage(revokeResult.data, "吊销失败。"));
                  return;
                }
                showError(nodes.deviceFeedback, "");
                loadDevices();
              });
            });
            item.appendChild(button);
          }
          nodes.deviceList.appendChild(item);
        }(devices[index]));
      }
    });
  }

  function handleDeviceCreate() {
    var label = nodes.deviceLabel.value.trim() || "passport";
    api("POST", "api/v1/devices", { label: label }).then(function (result) {
      if (!result.ok || !result.data) {
        showError(nodes.deviceFeedback, errorMessage(result.data, "生成设备凭据失败。"));
        return;
      }
      showError(nodes.deviceFeedback, "");
      nodes.deviceToken.hidden = false;
      nodes.deviceToken.textContent = "仅显示一次，请立即粘贴到设备：" + result.data.token;
      loadDevices();
    });
  }

  function handlePasswordSubmit(event) {
    event.preventDefault();
    var current = nodes.passwordCurrent.value;
    var replacement = nodes.passwordNew.value;
    if (replacement.length < 8) {
      showError(nodes.passwordFeedback, "新密码至少 8 位。");
      return;
    }
    api("POST", "api/v1/auth/change-password", {
      current_password: current,
      new_password: replacement
    }).then(function (result) {
      if (!result.ok) {
        showError(nodes.passwordFeedback, errorMessage(result.data, "修改密码失败。"));
        return;
      }
      nodes.passwordForm.reset();
      showNotice(nodes.passwordFeedback, "密码已修改。");
    });
  }

  /* ------------------------------------------------------------------ */
  /* Wiring                                                              */
  /* ------------------------------------------------------------------ */

  function wireEvents() {
    nodes.loginForm.addEventListener("submit", handleLogin);
    nodes.recordCategory.addEventListener("change", syncCategoryFields);
    nodes.recordSubitem.addEventListener("change", syncCategoryFields);
    nodes.recordForm.addEventListener("submit", handleRecordSubmit);
    nodes.recordRetry.addEventListener("click", function (event) {
      handleRecordSubmit(event);
    });
    nodes.overviewRecord.addEventListener("click", function () {
      window.location.hash = "#/record";
    });
    nodes.filterApply.addEventListener("click", applyFilters);
    nodes.recordMore.addEventListener("click", function () {
      loadRecordList(false);
    });
    nodes.detailEdit.addEventListener("click", handleDetailEdit);
    nodes.detailRevoke.addEventListener("click", handleDetailRevoke);
    nodes.profileForm.addEventListener("submit", handleProfileSubmit);
    nodes.passwordForm.addEventListener("submit", handlePasswordSubmit);
    nodes.deviceCreate.addEventListener("click", handleDeviceCreate);
    nodes.logoutButton.addEventListener("click", handleLogout);
    window.addEventListener("hashchange", function () {
      render(parseHash());
    });
  }

  function init() {
    cacheNodes();
    fillCategories();
    wireEvents();
    resetRecordForm();
    ensureSession().then(function (authenticated) {
      if (authenticated) {
        if (window.location.hash === "" || window.location.hash === "#/login") {
          window.location.hash = "#/overview";
        }
      } else {
        window.location.hash = "#/login";
      }
      render(parseHash());
    });
  }

  if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", init);
  } else {
    init();
  }
}());
