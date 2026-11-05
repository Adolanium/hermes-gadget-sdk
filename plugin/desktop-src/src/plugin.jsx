// Hermes Gadget — Desktop half. A Gadgets page (route, sidebar row, palette command) with a virtual
// gadget that speaks the device protocol to this Hermes, and live mirrors of real gadgets. Its backend
// is the package's dashboard/plugin_api.py (the same routes the web dashboard's Gadgets tab uses).
import { host, PALETTE_AREA, ROUTES_AREA, SIDEBAR_NAV_AREA } from '@hermes/plugin-sdk'
import { GadgetsPage } from './ui/page.jsx'

const PATH = '/gadgets'

export default {
  id: 'gadget',
  name: 'Gadgets',
  register(ctx) {
    const rest = (path, opts) => ctx.rest(path, opts)
    ctx.registerMany([
      {
        id: 'page',
        area: ROUTES_AREA,
        title: 'Gadgets',
        data: { path: PATH },
        render: () => <GadgetsPage rest={rest} storage={ctx.storage} />
      },
      {
        id: 'nav',
        area: SIDEBAR_NAV_AREA,
        data: { path: PATH, label: 'Gadgets', codicon: 'device-mobile' }
      },
      {
        id: 'open',
        area: PALETTE_AREA,
        data: {
          id: 'gadget.open',
          label: 'Open Gadgets',
          keywords: ['gadget', 'device', 'esp32', 'voice', 'hardware', 'talk', 'pair'],
          run: () => host.navigate(PATH)
        }
      }
    ])
  }
}
