import { defineConfig } from 'vitepress'

export default defineConfig({
  title: 'QtALH',
  description: 'Guides and reference for operating, configuring, and developing the Qt EPICS Alarm Handler.',
  lang: 'en-US',
  base: process.env.DOCS_BASE || '/',
  outDir: './dist',
  cleanUrls: false,
  head: [['meta', { name: 'theme-color', content: '#123d52' }]],
  markdown: { lineNumbers: false },
  themeConfig: {
    siteTitle: 'QtALH / docs',
    search: { provider: 'local' },
    outline: { level: [2, 3], label: 'On this page' },
    nav: [
      { text: 'Guide', link: '/get-started/first-run' },
      { text: 'Reference', link: '/reference/command-line' },
      { text: 'Development', link: '/develop/architecture' },
      { text: 'Repository', link: 'https://github.com/rtsoliday/qtalh' }
    ],
    sidebar: [
      { text: 'GET STARTED', items: [
        { text: 'Overview', link: '/' },
        { text: 'Install & build', link: '/get-started/install' },
        { text: 'Open your first configuration', link: '/get-started/first-run' },
        { text: 'Your first alarm', link: '/get-started/first-alarm' }
      ]},
      { text: 'OPERATE', collapsed: false, items: [
        { text: 'Monitor & acknowledge', link: '/operate/alarms' },
        { text: 'Appearance & font size', link: '/operate/appearance' },
        { text: 'Silence & display filters', link: '/operate/silence-filters' },
        { text: 'Timed shelving', link: '/operate/shelving' },
        { text: 'Notifications & escalation', link: '/operate/notifications' },
        { text: 'Alarm analytics', link: '/operate/analytics' },
        { text: 'Read alarm logs', link: '/operate/logs' },
        { text: 'Troubleshooting', link: '/operate/troubleshooting' }
      ]},
      { text: 'CONFIGURE', collapsed: true, items: [
        { text: 'Create & edit a configuration', link: '/configure/editor' },
        { text: 'Masks & automatic forcing', link: '/configure/masks' },
        { text: 'Logging & shared operation', link: '/configure/logging' }
      ]},
      { text: 'REFERENCE', collapsed: true, items: [
        { text: 'Command-line options', link: '/reference/command-line' },
        { text: 'Configuration format', link: '/reference/configuration' },
        { text: 'Environment variables', link: '/reference/environment' },
        { text: 'Helper programs', link: '/reference/helpers' }
      ]},
      { text: 'UNDERSTAND & DEVELOP', collapsed: true, items: [
        { text: 'Alarm lifecycle & modes', link: '/understand/alarm-lifecycle' },
        { text: 'Architecture', link: '/develop/architecture' },
        { text: 'Tests & contributions', link: '/develop/testing' },
        { text: 'Maintain these docs', link: '/develop/documentation' },
        { text: 'Compatibility', link: '/understand/compatibility' }
      ]},
      { text: 'PROJECT & HISTORY', collapsed: true, items: [
        { text: 'Authors', link: '/project/authors' },
        { text: 'License', link: '/project/license' },
        { text: 'Engineering history', link: '/history/' },
        { text: 'CPU benchmarks', link: '/history/performance' },
        { text: 'Logging investigation', link: '/history/logging' },
        { text: 'Appearance comparisons', link: '/history/appearance' },
        { text: 'Original ALH manual', link: '/legacy/ALH.html', target: '_self' }
      ]}
    ],
    footer: { message: 'Qt port development & maintenance: Robert Soliday', copyright: 'Based on EPICS ALH 1.2.35. Original author credits and license preserved.' }
  }
})
