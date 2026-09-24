import { createI18n } from "vue-i18n";

export const LOCALE_STORAGE_KEY = "metalsharp-locale";

export const localeOptions = [
  { code: "en", label: "English", native: "English" },
  { code: "zh-CN", label: "Chinese (Simplified)", native: "简体中文" },
  { code: "es", label: "Spanish", native: "Español" },
  { code: "hi", label: "Hindi", native: "हिन्दी" },
  { code: "ar", label: "Arabic", native: "العربية" },
  { code: "pt-BR", label: "Portuguese (Brazil)", native: "Português (Brasil)" },
  { code: "bn", label: "Bengali", native: "বাংলা" },
  { code: "ru", label: "Russian", native: "Русский" },
  { code: "ja", label: "Japanese", native: "日本語" },
  { code: "pa", label: "Punjabi", native: "ਪੰਜਾਬੀ" },
  { code: "de", label: "German", native: "Deutsch" },
  { code: "jv", label: "Javanese", native: "Basa Jawa" },
  { code: "ko", label: "Korean", native: "한국어" },
  { code: "fr", label: "French", native: "Français" },
  { code: "te", label: "Telugu", native: "తెలుగు" },
  { code: "vi", label: "Vietnamese", native: "Tiếng Việt" },
  { code: "tr", label: "Turkish", native: "Türkçe" },
  { code: "ur", label: "Urdu", native: "اردو" },
  { code: "it", label: "Italian", native: "Italiano" },
  { code: "mr", label: "Marathi", native: "मराठी" },
] as const;

const english = {
  language: { label: "Language", choose: "Choose language" },
  nav: { library: "Library", sharp: "Sharp Library", logs: "Logs", settings: "Settings" },
  actions: { back: "Back", next: "Next Step", close: "Close", save: "Save", cancel: "Cancel", done: "Done" },
  setup: {
    steps: ["Welcome", "Runtime", "Done"],
    titles: ["Welcome to MetalSharp", "Install Runtime", "You're All Set!"],
    taglines: ["Your Windows games. At home on Mac.", "One download, everything translated.", "MetalSharp is ready."],
    lede: "Play Windows Steam games on Apple Silicon.",
    features: {
      directx: "DirectX 9/10/11/12 Support",
      directxDesc: "Windows graphics, translated for Metal.",
      fna: "FNA & XNA",
      fnaDesc: "Native Mono support for your games.",
      steam: "Steam integration",
      steamDesc: "Browse, install, and launch your library.",
    },
    runtimeLede: "Installs the Wine runtime, graphics runtimes, Steam support files, and Mono/FNA support. GPTK is not installed during first-time setup.",
    bundledTools: "Bundled tools — no Homebrew required",
    toolExtraction: "Runtime bundle extraction",
    toolRar: "RAR archive extraction",
    toolIcons: "Windows icon extraction",
    toolArchives: "Safe archive inspection and extraction",
    installRuntime: "Install Runtime",
    installComplete: "Install Complete",
    installFailed: "Install Failed",
    installLog: "Install Log",
    installSteam: "Install Steam",
    steamInstalled: "Steam Installed",
    steamFailed: "Steam Install Failed",
    startSteamHint: "Please run “Start Steam” after installing",
    deviceName: "Device Name",
    devicePlaceholder: "e.g. Swift-Falcon",
    deviceHint: "Identifies your machine to Steam for persistent login.",
    apiKey: "Steam Web API Key (optional)",
    apiPlaceholder: "Enter your Steam Web API key...",
    apiHint: "Loads your full game library. Get a free key at",
    startSteam: "Start Steam",
    firstLaunch: "First launch",
    firstLaunchText: "MetalSharp auto-configures the runtime for each game. Optionally, configure a different setting using the bottle selection dropdown.",
    preparing: "Preparing Steam...",
    getStarted: "Get Started",
    launch: "Launch MetalSharp",
    exit: "Exit setup",
    stepOf: "Step {step} of {total}",
    downloadingSteam: "Downloading Steam...",
    creatingSteamPrefix: "Creating Steam prefix...",
    installingSteam: "Installing Steam...",
    retrySteam: "Retry Steam installation",
    preparingSteam: "Preparing Steam...",
    steamInstallFailed: "Failed to install Steam",
    steamInstallTimedOut: "Steam installation timed out",
    wrapperWarning: "Steam wrapper shims could not be verified; MetalSharp will retry before Steam launch.",
    apiKeySaveFailed: "Failed to save Steam API key",
    apiKeySteamIdMissing: "API key saved, but SteamID was not detected yet",
    startSteamText: "Click Start Steam in your Library, then log in through the Steam window.",
  },
  settings: {
    title: "Settings",
    language: "Language",
    languageDesc: "Choose the language used throughout MetalSharp.",
    steamIntegration: "Steam Integration",
    steam: "Steam",
    backend: "Backend",
    dataPermissions: "Data & Permissions",
    cache: "Cache",
    updates: "Updates",
  },
} as const;

type LocaleMessages = Record<string, unknown>;

const translations: Record<string, LocaleMessages> = {
  "zh-CN": {
    language: { label: "语言", choose: "选择语言" },
    nav: { library: "游戏库", sharp: "Sharp 游戏库", logs: "日志", settings: "设置" },
    actions: { back: "返回", next: "下一步", close: "关闭", save: "保存", cancel: "取消", done: "完成" },
    setup: { steps: ["欢迎", "运行环境", "完成"], titles: ["欢迎使用 MetalSharp", "安装运行环境", "准备完成！"], taglines: ["让 Windows 游戏在 Mac 上运行。", "一次下载，全部完成转换。", "MetalSharp 已准备就绪。"], lede: "在 Apple Silicon 上畅玩 Windows Steam 游戏。", installRuntime: "安装运行环境", installComplete: "安装完成", installFailed: "安装失败", installLog: "安装日志", installSteam: "安装 Steam", steamInstalled: "Steam 已安装", deviceName: "设备名称", apiKey: "Steam Web API 密钥（可选）", startSteam: "启动 Steam", getStarted: "开始使用", launch: "启动 MetalSharp", exit: "退出设置" },
    settings: { title: "设置", language: "语言", languageDesc: "选择 MetalSharp 使用的语言。", steamIntegration: "Steam 集成", steam: "Steam", backend: "后端", dataPermissions: "数据与权限", cache: "缓存", updates: "更新" },
  },
  es: { language: { label: "Idioma", choose: "Elegir idioma" }, nav: { library: "Biblioteca", sharp: "Biblioteca Sharp", logs: "Registros", settings: "Ajustes" }, actions: { back: "Atrás", next: "Siguiente", close: "Cerrar", save: "Guardar", cancel: "Cancelar", done: "Listo" }, setup: { steps: ["Bienvenida", "Runtime", "Listo"], titles: ["Bienvenido a MetalSharp", "Instalar runtime", "¡Todo listo!"], lede: "Juega títulos de Windows en Apple Silicon.", installRuntime: "Instalar runtime", installComplete: "Instalación completa", installFailed: "Instalación fallida", installSteam: "Instalar Steam", deviceName: "Nombre del dispositivo", apiKey: "Clave de Steam Web API (opcional)", getStarted: "Comenzar", launch: "Iniciar MetalSharp", exit: "Salir de la configuración" }, settings: { title: "Ajustes", language: "Idioma", languageDesc: "Elige el idioma de MetalSharp.", steamIntegration: "Integración con Steam", steam: "Steam", backend: "Backend", dataPermissions: "Datos y permisos", cache: "Caché", updates: "Actualizaciones" } },
  hi: { language: { label: "भाषा", choose: "भाषा चुनें" }, nav: { library: "लाइब्रेरी", sharp: "Sharp लाइब्रेरी", logs: "लॉग", settings: "सेटिंग्स" }, actions: { back: "वापस", next: "अगला चरण", close: "बंद करें", save: "सहेजें", cancel: "रद्द करें", done: "पूर्ण" }, setup: { steps: ["स्वागत", "रनटाइम", "पूर्ण"], titles: ["MetalSharp में आपका स्वागत है", "रनटाइम इंस्टॉल करें", "सब तैयार है!"], lede: "Apple Silicon पर Windows Steam गेम खेलें।", installRuntime: "रनटाइम इंस्टॉल करें", installComplete: "इंस्टॉल पूरा", installFailed: "इंस्टॉल विफल", installSteam: "Steam इंस्टॉल करें", deviceName: "डिवाइस का नाम", getStarted: "शुरू करें", launch: "MetalSharp शुरू करें", exit: "सेटअप से बाहर निकलें" }, settings: { title: "सेटिंग्स", language: "भाषा", languageDesc: "MetalSharp की भाषा चुनें।", steamIntegration: "Steam एकीकरण", steam: "Steam", backend: "बैकएंड", dataPermissions: "डेटा और अनुमतियाँ", cache: "कैश", updates: "अपडेट" } },
  ar: { language: { label: "اللغة", choose: "اختر اللغة" }, nav: { library: "المكتبة", sharp: "مكتبة Sharp", logs: "السجلات", settings: "الإعدادات" }, actions: { back: "رجوع", next: "الخطوة التالية", close: "إغلاق", save: "حفظ", cancel: "إلغاء", done: "تم" }, setup: { steps: ["الترحيب", "النظام", "تم"], titles: ["مرحبًا بك في MetalSharp", "تثبيت النظام", "كل شيء جاهز!"], lede: "شغّل ألعاب Windows على Apple Silicon.", installRuntime: "تثبيت النظام", installComplete: "اكتمل التثبيت", installFailed: "فشل التثبيت", installSteam: "تثبيت Steam", deviceName: "اسم الجهاز", getStarted: "ابدأ", launch: "تشغيل MetalSharp", exit: "الخروج من الإعداد" }, settings: { title: "الإعدادات", language: "اللغة", languageDesc: "اختر لغة MetalSharp.", steamIntegration: "تكامل Steam", steam: "Steam", backend: "الخلفية", dataPermissions: "البيانات والأذونات", cache: "ذاكرة التخزين المؤقت", updates: "التحديثات" } },
  "pt-BR": { language: { label: "Idioma", choose: "Escolher idioma" }, nav: { library: "Biblioteca", sharp: "Biblioteca Sharp", logs: "Registros", settings: "Configurações" }, actions: { back: "Voltar", next: "Próxima etapa", close: "Fechar", save: "Salvar", cancel: "Cancelar", done: "Concluído" }, setup: { steps: ["Boas-vindas", "Runtime", "Concluído"], titles: ["Bem-vindo ao MetalSharp", "Instalar runtime", "Tudo pronto!"], lede: "Jogue títulos Windows no Apple Silicon.", installRuntime: "Instalar runtime", installComplete: "Instalação concluída", installFailed: "Falha na instalação", installSteam: "Instalar Steam", deviceName: "Nome do dispositivo", getStarted: "Começar", launch: "Abrir MetalSharp", exit: "Sair da configuração" }, settings: { title: "Configurações", language: "Idioma", languageDesc: "Escolha o idioma do MetalSharp.", steamIntegration: "Integração com Steam", steam: "Steam", backend: "Backend", dataPermissions: "Dados e permissões", cache: "Cache", updates: "Atualizações" } },
  bn: { language: { label: "ভাষা", choose: "ভাষা বেছে নিন" }, nav: { library: "লাইব্রেরি", sharp: "Sharp লাইব্রেরি", logs: "লগ", settings: "সেটিংস" }, actions: { back: "পিছনে", next: "পরের ধাপ", close: "বন্ধ", save: "সংরক্ষণ", cancel: "বাতিল", done: "সম্পন্ন" }, setup: { steps: ["স্বাগতম", "রানটাইম", "সম্পন্ন"], titles: ["MetalSharp-এ স্বাগতম", "রানটাইম ইনস্টল করুন", "সব প্রস্তুত!"], lede: "Apple Silicon-এ Windows Steam গেম খেলুন।", installRuntime: "রানটাইম ইনস্টল", installComplete: "ইনস্টল সম্পন্ন", installFailed: "ইনস্টল ব্যর্থ", installSteam: "Steam ইনস্টল", deviceName: "ডিভাইসের নাম", getStarted: "শুরু করুন", launch: "MetalSharp চালু করুন", exit: "সেটআপ থেকে বের হন" }, settings: { title: "সেটিংস", language: "ভাষা", languageDesc: "MetalSharp-এর ভাষা বেছে নিন।", steamIntegration: "Steam ইন্টিগ্রেশন", steam: "Steam", backend: "ব্যাকএন্ড", dataPermissions: "ডেটা ও অনুমতি", cache: "ক্যাশ", updates: "আপডেট" } },
  ru: { language: { label: "Язык", choose: "Выберите язык" }, nav: { library: "Библиотека", sharp: "Библиотека Sharp", logs: "Журналы", settings: "Настройки" }, actions: { back: "Назад", next: "Далее", close: "Закрыть", save: "Сохранить", cancel: "Отмена", done: "Готово" }, setup: { steps: ["Добро пожаловать", "Среда", "Готово"], titles: ["Добро пожаловать в MetalSharp", "Установка среды", "Всё готово!"], lede: "Играйте в Windows-игры на Apple Silicon.", installRuntime: "Установить среду", installComplete: "Установка завершена", installFailed: "Ошибка установки", installSteam: "Установить Steam", deviceName: "Имя устройства", getStarted: "Начать", launch: "Запустить MetalSharp", exit: "Выйти из настройки" }, settings: { title: "Настройки", language: "Язык", languageDesc: "Выберите язык MetalSharp.", steamIntegration: "Интеграция Steam", steam: "Steam", backend: "Бэкенд", dataPermissions: "Данные и разрешения", cache: "Кэш", updates: "Обновления" } },
  ja: { language: { label: "言語", choose: "言語を選択" }, nav: { library: "ライブラリ", sharp: "Sharp ライブラリ", logs: "ログ", settings: "設定" }, actions: { back: "戻る", next: "次へ", close: "閉じる", save: "保存", cancel: "キャンセル", done: "完了" }, setup: { steps: ["ようこそ", "ランタイム", "完了"], titles: ["MetalSharp へようこそ", "ランタイムをインストール", "準備完了！"], lede: "Apple Silicon で Windows Steam ゲームをプレイ。", installRuntime: "ランタイムをインストール", installComplete: "インストール完了", installFailed: "インストール失敗", installSteam: "Steam をインストール", deviceName: "デバイス名", getStarted: "始める", launch: "MetalSharp を起動", exit: "設定を終了" }, settings: { title: "設定", language: "言語", languageDesc: "MetalSharp の言語を選択します。", steamIntegration: "Steam 連携", steam: "Steam", backend: "バックエンド", dataPermissions: "データと権限", cache: "キャッシュ", updates: "アップデート" } },
  de: { language: { label: "Sprache", choose: "Sprache wählen" }, nav: { library: "Bibliothek", sharp: "Sharp-Bibliothek", logs: "Protokolle", settings: "Einstellungen" }, actions: { back: "Zurück", next: "Nächster Schritt", close: "Schließen", save: "Speichern", cancel: "Abbrechen", done: "Fertig" }, setup: { steps: ["Willkommen", "Laufzeit", "Fertig"], titles: ["Willkommen bei MetalSharp", "Laufzeit installieren", "Alles bereit!"], lede: "Windows-Spiele auf Apple Silicon spielen.", installRuntime: "Laufzeit installieren", installComplete: "Installation abgeschlossen", installFailed: "Installation fehlgeschlagen", installSteam: "Steam installieren", deviceName: "Gerätename", getStarted: "Loslegen", launch: "MetalSharp starten", exit: "Setup verlassen" }, settings: { title: "Einstellungen", language: "Sprache", languageDesc: "Wähle die Sprache für MetalSharp.", steamIntegration: "Steam-Integration", steam: "Steam", backend: "Backend", dataPermissions: "Daten & Berechtigungen", cache: "Cache", updates: "Updates" } },
  ko: { language: { label: "언어", choose: "언어 선택" }, nav: { library: "라이브러리", sharp: "Sharp 라이브러리", logs: "로그", settings: "설정" }, actions: { back: "뒤로", next: "다음 단계", close: "닫기", save: "저장", cancel: "취소", done: "완료" }, setup: { steps: ["환영합니다", "런타임", "완료"], titles: ["MetalSharp에 오신 것을 환영합니다", "런타임 설치", "준비 완료!"], lede: "Apple Silicon에서 Windows Steam 게임을 플레이하세요.", installRuntime: "런타임 설치", installComplete: "설치 완료", installFailed: "설치 실패", installSteam: "Steam 설치", deviceName: "기기 이름", getStarted: "시작", launch: "MetalSharp 실행", exit: "설정 종료" }, settings: { title: "설정", language: "언어", languageDesc: "MetalSharp의 언어를 선택하세요.", steamIntegration: "Steam 통합", steam: "Steam", backend: "백엔드", dataPermissions: "데이터 및 권한", cache: "캐시", updates: "업데이트" } },
  fr: { language: { label: "Langue", choose: "Choisir la langue" }, nav: { library: "Bibliothèque", sharp: "Bibliothèque Sharp", logs: "Journaux", settings: "Paramètres" }, actions: { back: "Retour", next: "Étape suivante", close: "Fermer", save: "Enregistrer", cancel: "Annuler", done: "Terminé" }, setup: { steps: ["Bienvenue", "Runtime", "Terminé"], titles: ["Bienvenue dans MetalSharp", "Installer le runtime", "Tout est prêt !"], lede: "Jouez aux jeux Windows sur Apple Silicon.", installRuntime: "Installer le runtime", installComplete: "Installation terminée", installFailed: "Échec de l'installation", installSteam: "Installer Steam", deviceName: "Nom de l'appareil", getStarted: "Commencer", launch: "Lancer MetalSharp", exit: "Quitter la configuration" }, settings: { title: "Paramètres", language: "Langue", languageDesc: "Choisissez la langue de MetalSharp.", steamIntegration: "Intégration Steam", steam: "Steam", backend: "Backend", dataPermissions: "Données et autorisations", cache: "Cache", updates: "Mises à jour" } },
  vi: { language: { label: "Ngôn ngữ", choose: "Chọn ngôn ngữ" }, nav: { library: "Thư viện", sharp: "Thư viện Sharp", logs: "Nhật ký", settings: "Cài đặt" }, actions: { back: "Quay lại", next: "Bước tiếp theo", close: "Đóng", save: "Lưu", cancel: "Hủy", done: "Xong" }, setup: { steps: ["Chào mừng", "Runtime", "Hoàn tất"], titles: ["Chào mừng đến MetalSharp", "Cài đặt runtime", "Đã sẵn sàng!"], lede: "Chơi game Windows Steam trên Apple Silicon.", installRuntime: "Cài đặt runtime", installComplete: "Đã cài đặt", installFailed: "Cài đặt thất bại", installSteam: "Cài đặt Steam", deviceName: "Tên thiết bị", getStarted: "Bắt đầu", launch: "Mở MetalSharp", exit: "Thoát thiết lập" }, settings: { title: "Cài đặt", language: "Ngôn ngữ", languageDesc: "Chọn ngôn ngữ cho MetalSharp.", steamIntegration: "Tích hợp Steam", steam: "Steam", backend: "Backend", dataPermissions: "Dữ liệu và quyền", cache: "Bộ nhớ đệm", updates: "Cập nhật" } },
  tr: { language: { label: "Dil", choose: "Dil seçin" }, nav: { library: "Kütüphane", sharp: "Sharp Kütüphanesi", logs: "Günlükler", settings: "Ayarlar" }, actions: { back: "Geri", next: "Sonraki adım", close: "Kapat", save: "Kaydet", cancel: "İptal", done: "Bitti" }, setup: { steps: ["Hoş geldiniz", "Çalışma zamanı", "Bitti"], titles: ["MetalSharp'a hoş geldiniz", "Çalışma zamanını yükle", "Her şey hazır!"], lede: "Windows Steam oyunlarını Apple Silicon'da oynayın.", installRuntime: "Çalışma zamanını yükle", installComplete: "Kurulum tamamlandı", installFailed: "Kurulum başarısız", installSteam: "Steam'i yükle", deviceName: "Cihaz adı", getStarted: "Başla", launch: "MetalSharp'i başlat", exit: "Kurulumdan çık" }, settings: { title: "Ayarlar", language: "Dil", languageDesc: "MetalSharp dilini seçin.", steamIntegration: "Steam entegrasyonu", steam: "Steam", backend: "Arka uç", dataPermissions: "Veriler ve izinler", cache: "Önbellek", updates: "Güncellemeler" } },
  it: { language: { label: "Lingua", choose: "Scegli lingua" }, nav: { library: "Libreria", sharp: "Libreria Sharp", logs: "Log", settings: "Impostazioni" }, actions: { back: "Indietro", next: "Passo successivo", close: "Chiudi", save: "Salva", cancel: "Annulla", done: "Fatto" }, setup: { steps: ["Benvenuto", "Runtime", "Fatto"], titles: ["Benvenuto in MetalSharp", "Installa runtime", "Tutto pronto!"], lede: "Gioca ai titoli Windows su Apple Silicon.", installRuntime: "Installa runtime", installComplete: "Installazione completata", installFailed: "Installazione non riuscita", installSteam: "Installa Steam", deviceName: "Nome dispositivo", getStarted: "Inizia", launch: "Avvia MetalSharp", exit: "Esci dalla configurazione" }, settings: { title: "Impostazioni", language: "Lingua", languageDesc: "Scegli la lingua di MetalSharp.", steamIntegration: "Integrazione Steam", steam: "Steam", backend: "Backend", dataPermissions: "Dati e permessi", cache: "Cache", updates: "Aggiornamenti" } },
  mr: { language: { label: "भाषा", choose: "भाषा निवडा" }, nav: { library: "लायब्ररी", sharp: "Sharp लायब्ररी", logs: "लॉग", settings: "सेटिंग्ज" }, actions: { back: "मागे", next: "पुढील पायरी", close: "बंद करा", save: "जतन करा", cancel: "रद्द करा", done: "पूर्ण" }, setup: { steps: ["स्वागत", "रनटाइम", "पूर्ण"], titles: ["MetalSharp मध्ये स्वागत आहे", "रनटाइम स्थापित करा", "सर्व तयार!"], lede: "Apple Silicon वर Windows Steam गेम खेळा.", installRuntime: "रनटाइम स्थापित करा", installComplete: "स्थापना पूर्ण", installFailed: "स्थापना अयशस्वी", installSteam: "Steam स्थापित करा", deviceName: "डिव्हाइसचे नाव", getStarted: "सुरुवात करा", launch: "MetalSharp सुरू करा", exit: "सेटअपमधून बाहेर पडा" }, settings: { title: "सेटिंग्ज", language: "भाषा", languageDesc: "MetalSharp ची भाषा निवडा.", steamIntegration: "Steam एकत्रीकरण", steam: "Steam", backend: "बॅकएंड", dataPermissions: "डेटा आणि परवानग्या", cache: "कॅशे", updates: "अपडेट्स" } },
  te: { language: { label: "భాష", choose: "భాషను ఎంచుకోండి" }, nav: { library: "లైబ్రరీ", sharp: "Sharp లైబ్రరీ", logs: "లాగ్‌లు", settings: "సెట్టింగ్‌లు" }, actions: { back: "వెనుకకు", next: "తదుపరి దశ", close: "మూసివేయి", save: "సేవ్ చేయి", cancel: "రద్దు చేయి", done: "పూర్తయింది" }, setup: { steps: ["స్వాగతం", "రన్‌టైమ్", "పూర్తయింది"], titles: ["MetalSharpకు స్వాగతం", "రన్‌టైమ్ ఇన్‌స్టాల్ చేయండి", "అన్నీ సిద్ధం!"], lede: "Apple Siliconలో Windows Steam గేమ్‌లు ఆడండి.", installRuntime: "రన్‌టైమ్ ఇన్‌స్టాల్", installComplete: "ఇన్‌స్టాల్ పూర్తయింది", installFailed: "ఇన్‌స్టాల్ విఫలమైంది", installSteam: "Steam ఇన్‌స్టాల్", deviceName: "పరికరం పేరు", getStarted: "ప్రారంభించండి", launch: "MetalSharp ప్రారంభించండి", exit: "సెటప్ నుండి నిష్క్రమించండి" }, settings: { title: "సెట్టింగ్‌లు", language: "భాష", languageDesc: "MetalSharp భాషను ఎంచుకోండి.", steamIntegration: "Steam అనుసంధానం", steam: "Steam", backend: "బ్యాకెండ్", dataPermissions: "డేటా మరియు అనుమతులు", cache: "క్యాష్", updates: "నవీకరణలు" } },
  ur: { language: { label: "زبان", choose: "زبان منتخب کریں" }, nav: { library: "لائبریری", sharp: "Sharp لائبریری", logs: "لاگز", settings: "ترتیبات" }, actions: { back: "واپس", next: "اگلا مرحلہ", close: "بند کریں", save: "محفوظ کریں", cancel: "منسوخ", done: "مکمل" }, setup: { steps: ["خوش آمدید", "رن ٹائم", "مکمل"], titles: ["MetalSharp میں خوش آمدید", "رن ٹائم انسٹال کریں", "سب تیار ہے!"], lede: "Apple Silicon پر Windows Steam گیمز کھیلیں۔", installRuntime: "رن ٹائم انسٹال کریں", installComplete: "انسٹالیشن مکمل", installFailed: "انسٹالیشن ناکام", installSteam: "Steam انسٹال کریں", deviceName: "ڈیوائس کا نام", getStarted: "شروع کریں", launch: "MetalSharp چلائیں", exit: "سیٹ اپ سے باہر نکلیں" }, settings: { title: "ترتیبات", language: "زبان", languageDesc: "MetalSharp کی زبان منتخب کریں۔", steamIntegration: "Steam انضمام", steam: "Steam", backend: "بیک اینڈ", dataPermissions: "ڈیٹا اور اجازتیں", cache: "کیش", updates: "اپ ڈیٹس" } },
  pa: { language: { label: "ਭਾਸ਼ਾ", choose: "ਭਾਸ਼ਾ ਚੁਣੋ" }, nav: { library: "ਲਾਇਬ੍ਰੇਰੀ", sharp: "Sharp ਲਾਇਬ੍ਰੇਰੀ", logs: "ਲੌਗ", settings: "ਸੈਟਿੰਗਾਂ" }, actions: { back: "ਪਿੱਛੇ", next: "ਅਗਲਾ ਕਦਮ", close: "ਬੰਦ ਕਰੋ", save: "ਸੰਭਾਲੋ", cancel: "ਰੱਦ ਕਰੋ", done: "ਮੁਕੰਮਲ" }, setup: { steps: ["ਸਵਾਗਤ", "ਰਨਟਾਈਮ", "ਮੁਕੰਮਲ"], titles: ["MetalSharp ਵਿੱਚ ਜੀ ਆਇਆਂ ਨੂੰ", "ਰਨਟਾਈਮ ਇੰਸਟਾਲ ਕਰੋ", "ਸਭ ਤਿਆਰ!"], lede: "Apple Silicon ਉੱਤੇ Windows Steam ਗੇਮਾਂ ਖੇਡੋ।", installRuntime: "ਰਨਟਾਈਮ ਇੰਸਟਾਲ ਕਰੋ", installComplete: "ਇੰਸਟਾਲੇਸ਼ਨ ਮੁਕੰਮਲ", installFailed: "ਇੰਸਟਾਲੇਸ਼ਨ ਅਸਫਲ", installSteam: "Steam ਇੰਸਟਾਲ ਕਰੋ", deviceName: "ਡਿਵਾਈਸ ਦਾ ਨਾਮ", getStarted: "ਸ਼ੁਰੂ ਕਰੋ", launch: "MetalSharp ਚਲਾਓ", exit: "ਸੈਟਅੱਪ ਤੋਂ ਬਾਹਰ" }, settings: { title: "ਸੈਟਿੰਗਾਂ", language: "ਭਾਸ਼ਾ", languageDesc: "MetalSharp ਦੀ ਭਾਸ਼ਾ ਚੁਣੋ।", steamIntegration: "Steam ਏਕੀਕਰਨ", steam: "Steam", backend: "ਬੈਕਐਂਡ", dataPermissions: "ਡਾਟਾ ਅਤੇ ਇਜਾਜ਼ਤਾਂ", cache: "ਕੈਸ਼", updates: "ਅੱਪਡੇਟ" } },
  jv: { language: { label: "Basa", choose: "Pilih basa" }, nav: { library: "Pustaka", sharp: "Pustaka Sharp", logs: "Log", settings: "Setelan" }, actions: { back: "Bali", next: "Langkah sabanjure", close: "Tutup", save: "Simpen", cancel: "Batal", done: "Rampung" }, setup: { steps: ["Sugeng rawuh", "Runtime", "Rampung"], titles: ["Sugeng rawuh ing MetalSharp", "Instal runtime", "Kabeh wis siap!"], lede: "Main game Windows Steam ing Apple Silicon.", installRuntime: "Instal runtime", installComplete: "Instalasi rampung", installFailed: "Instalasi gagal", installSteam: "Instal Steam", deviceName: "Jeneng piranti", getStarted: "Miwiti", launch: "Bukak MetalSharp", exit: "Metu saka persiyapan" }, settings: { title: "Setelan", language: "Basa", languageDesc: "Pilih basa kanggo MetalSharp.", steamIntegration: "Integrasi Steam", steam: "Steam", backend: "Backend", dataPermissions: "Data lan ijin", cache: "Cache", updates: "Pembaruan" } },
};

export const messages = Object.fromEntries(
  localeOptions.map(({ code }) => [code, code === "en" ? english : translations[code] ?? {}]),
);

function initialLocale(): string {
  const saved = localStorage.getItem(LOCALE_STORAGE_KEY);
  return localeOptions.some((locale) => locale.code === saved) ? saved! : "en";
}

export const i18n: any = createI18n({
  legacy: false,
  locale: initialLocale(),
  fallbackLocale: "en",
  messages: messages as any,
  missingWarn: false,
  fallbackWarn: false,
});

export function setAppLocale(locale: string): void {
  const next = localeOptions.some((option) => option.code === locale) ? locale : "en";
  i18n.global.locale.value = next;
  localStorage.setItem(LOCALE_STORAGE_KEY, next);
  document.documentElement.lang = next;
}
