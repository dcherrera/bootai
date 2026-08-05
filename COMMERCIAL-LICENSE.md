# Commercial Licensing

BootAI is licensed under the **GNU Affero General Public License v3.0** — see [`LICENSE`](./LICENSE).

Copyright © 2026 Corey Herrera. All rights reserved.

The AGPL is a strong copyleft license. If you build on BootAI and let anyone use the result — including over a network, which BootAI does by default the moment it serves its management API or advertises `bootai.local` — you must offer them the complete corresponding source of your application under the AGPL. For a lot of people that is fine. For some it is not.

This document describes the two ways to use BootAI **without** that obligation.

> **Scope.** These terms cover BootAI itself. `JACLibc/` is a third-party submodule under its own MIT license and is not mine to relicense. Model weights are covered separately — see "What neither option grants" below.

---

## 1\. Small Entity Exception — automatic, free, no paperwork

If you are a **Small Entity** (defined below), you may at your option use, modify and distribute BootAI under the terms of the **Apache License 2.0** instead of the AGPL-3.0.

You do not need to contact me, sign anything, or pay anything. This exception is granted in advance to everyone who qualifies.

### Who qualifies

You are a **Small Entity** if you are:

- an **individual** acting on your own behalf; or

- engaged in **academic or non-commercial research** use; or

- a **registered nonprofit, charity, or educational institution**; or

- an **organization** that, counted together with all of its parents, subsidiaries, and affiliates, meets **both** of:

  - fewer than **100** total employees and contractors, and

  - less than **USD $1,000,000** in gross revenue in its most recently completed fiscal year — or, for entities not measured by revenue (government bodies, for example), less than **USD $1,000,000** in total annual budget.

The affiliate counting is deliberate. A three-person subsidiary of a large organization is not a Small Entity.

### Once granted, it stays granted

If you qualified as a Small Entity at the time you received a given version of BootAI, your Apache-2.0 rights **in that version** are perpetual and irrevocable. Growing past the thresholds never retroactively strips rights you already had, and you never owe anything for what you already shipped.

It does mean that versions you obtain *after* you stop qualifying are AGPL-3.0, unless you obtain a commercial license.

---

## 2\. Commercial License — everyone else

If you exceed the Small Entity thresholds and you do not want the AGPL's source-disclosure obligations, you need a commercial license. It grants Apache-2.0-style terms — use, modify, embed, ship, serve, no copyleft — for a fee.

The usual reasons to want one:

- you **ship hardware** — appliances, kiosks, thin clients, industrial or embedded machines — that boots BootAI as its firmware payload;

- you **serve BootAI's management or chat API** as part of a product or service offered to others (the AGPL's network clause reaches this directly);

- you **embed** BootAI, or a derivative EFI binary built from it, in a product you distribute without source;

- your legal department does not permit AGPL-licensed dependencies (this is the common one).

**Contact:** `coreyh55@gmail.com`

Useful things to include in a first email: your organization, roughly how you intend to use BootAI, and whether you need distribution rights, hosted-service rights, or both.

---

## What neither option grants

- **Trademarks.** No right to use the "BootAI" or "TeamIDE" names or any associated logo to name or promote your own product. You may of course state factually that your product uses BootAI.

- **Patents beyond the grant** in whichever license applies to you (AGPL-3.0 §11 or Apache-2.0 §3).

- **Warranty or support.** BootAI is provided **as is**, without warranty of any kind. It writes to firmware variables and, if you ask it to, to your boot disk. A commercial license does not by itself include a support agreement; support is a separate arrangement.

- **Model weights.** This covers the boot firmware and inference runtime only. Models you run on it carry their own licenses, which are entirely between you and whoever published them.

- **Third-party submodules.** `JACLibc/` ships under MIT. Its terms travel with it and are unaffected by anything here.

---

## Notes

**Per-version terms.** The terms in this file apply to the version of BootAI they ship with. I may change the thresholds or terms for future releases. Any change is prospective only — see "Once granted, it stays granted" above.

**Contributions.** Contributions are accepted inbound under **Apache-2.0** (see [`CONTRIBUTING.md`](./CONTRIBUTING.md)). That is what keeps this dual-licensing arrangement workable: it lets contributed code be included in commercial builds alongside the rest. Contributors keep their copyright.

**This is not legal advice.** I wrote this to be clear and fair, not to be airtight under every jurisdiction's law. If you are making a decision that matters to your organization, have your own counsel read it. If something here is ambiguous, ask me rather than guessing — I would rather clarify it than have someone rely on a reading I did not intend.
