ArborTab — Pestañas en árbol en la barra vertical nativa de Chrome

Solo para Windows x64. Requiere la barra de pestañas vertical nativa de Chrome. La integración visual completa necesita un módulo nativo de código abierto instalado aparte desde GitHub. Lee el aviso de seguridad antes de instalarlo.

ArborTab organiza las pestañas de Chrome como un árbol real de padres e hijos dentro de la propia barra vertical de Chrome, sin barra lateral ni una segunda lista.

Funciones:

- Las pestañas abiertas desde enlaces pasan a ser hijas de la pestaña de origen.
- Contraer y expandir subárboles completos.
- Cerrar una pestaña padre junto con todos sus descendientes.
- Arrastrar una pestaña sobre otra para anidarla o soltarla en un borde para reordenar hermanas.
- Guías de sangría, contador de pestañas ocultas, colores del tema y animaciones opcionales.
- Atajos para contraer, cerrar un árbol, aumentar y reducir el nivel.

La extensión por sí sola mantiene el árbol, ordena pestañas, admite atajos y cierra subárboles. Sin el módulo nativo, la barra de Chrome sigue viéndose plana.

Instalación del módulo nativo:

Tras instalar la extensión, abre la configuración de ArborTab. Allí se enlaza la última versión de GitHub y se muestra el comando de instalación. Código, scripts, paquetes, sumas de comprobación y desinstalación:
https://github.com/EvATive7/chrome-plugin-arbortab

Comando de instalación:
powershell -NoExit -NoProfile -ExecutionPolicy Bypass -Command "irm https://github.com/EvATive7/chrome-plugin-arbortab/releases/latest/download/install.ps1 | iex"

Aviso de seguridad:

El componente nativo es un proxy `version.dll` instalado junto a `chrome.exe`. Se integra con el diseño, dibujo, ratón y arrastre de la barra vertical. Como la DLL distribuida no está firmada por Microsoft, desactiva para los procesos secundarios de Chrome la mitigación que solo permite binarios firmados por Microsoft. Esto reduce su protección de integridad de código. Instálalo únicamente si aceptas este compromiso. El paquete incluye desinstalador.

Privacidad:

ArborTab no incluye anuncios ni analítica, no opera un servidor y no sube datos de navegación. La extensión se comunica localmente mediante Native Messaging. El módulo descarga datos de compatibilidad firmados desde GitHub/jsDelivr, pero no sube URLs, títulos ni el árbol. Los informes de diagnóstico permanecen locales salvo que el usuario los exporte y comparta.
