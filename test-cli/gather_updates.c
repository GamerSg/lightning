#include <ccan/err/err.h>
#include <ccan/crypto/sha256/sha256.h>
#include <ccan/structeq/structeq.h>
#include "test-cli/gather_updates.h"
#include "commit_tx.h"
#include "funding.h"
#include "pkt.h"
#include "protobuf_convert.h"

static void check_preimage(const Sha256Hash *preimage,
			   const struct sha256 *old,
			   const struct sha256 *h,
			   const char *file)
{
	struct sha256 sha;

	if (!h)
		return;

	proto_to_sha256(preimage, &sha);
	sha256(&sha, &sha, sizeof(sha));
	if (!structeq(&sha, old))
		errx(1, "Invalid preimage in %s!", file);
}

/* Returns tal_count(oneside->htlcs) if not found. */
static size_t find_htlc(struct channel_oneside *oneside,
			const Sha256Hash *rhash)
{
	size_t i, n;
	struct sha256 h;

	proto_to_sha256(rhash, &h);
	
	n = tal_count(oneside->htlcs);
	for (i = 0; i < n; i++) {
		if (structeq(&oneside->htlcs[i].rhash, &h))
			break;
	}
	return i;
}

static void add_htlc(struct channel_oneside *oneside,
		     const UpdateAddHtlc *ah,
		     const char *file)
{
	size_t num = tal_count(oneside->htlcs);
	struct sha256 rhash;
	struct abs_locktime expiry;
	
	if (find_htlc(oneside, ah->r_hash) != num)
		errx(1, "Duplicate R hash in %s", file);

	proto_to_sha256(ah->r_hash, &rhash);
	proto_to_abs_locktime(ah->expiry, &expiry);
	funding_add_htlc(oneside, ah->amount_msat, &expiry, &rhash);
}

static void remove_htlc(struct channel_oneside *oneside, size_t n)
{
	size_t num = tal_count(oneside->htlcs);

	assert(n < num);

	/* Remove. */
	if (num > 0)
		oneside->htlcs[n] = oneside->htlcs[num-1];
	tal_resize(&oneside->htlcs, num - 1);
}

static void update_rhash(const Sha256Hash *rhash,
			 bool received,
			 size_t *num_updates,
			 struct sha256 *old_our_rhash,
			 struct sha256 *old_their_rhash,
			 struct sha256 *our_rhash,
			 struct sha256 *their_rhash)
{
	/* Update rhash (and save old one for checking) */
	if (received) {
		*old_their_rhash = *their_rhash;
		proto_to_sha256(rhash, their_rhash);
	} else {
		*old_our_rhash = *our_rhash;
		proto_to_sha256(rhash, our_rhash);
	}
	/* If they care, we count number of updates. */
	if (num_updates)
		(*num_updates)++;
}

static uint32_t htlcs_total(const struct channel_htlc *htlcs)
{
	size_t i, n = tal_count(htlcs);
	uint32_t total = 0;

	for (i = 0; i < n; i++)
		total += htlcs[i].msatoshis;
	return total;
}

/* Takes complete update history, gets summary of last state. */
struct channel_state *gather_updates(const tal_t *ctx,
			const OpenChannel *o1, const OpenChannel *o2,
			const OpenAnchor *oa, uint64_t fee,
			char **argv,
			size_t *num_updates,
			struct sha256 *our_rhash,
			struct sha256 *their_rhash,
			struct signature *their_commit_sig)
{
	Signature *sig = NULL;
	struct sha256 old_our_rhash, old_their_rhash, rhash1, rhash2;
	struct channel_state *cstate;
	
	/* Start sanity check. */
	if (is_funder(o1) == is_funder(o2))
		errx(1, "Must be exactly one funder");

	cstate = initial_funding(NULL, is_funder(o1), oa->amount, fee);
	if (!cstate)
		errx(1, "Invalid open combination (need to cover fees)");

	/* If they don't want these, use dummy ones. */
	if (!our_rhash)
		our_rhash = &rhash1;

	if (!their_rhash)
		their_rhash = &rhash2;

	proto_to_sha256(o1->revocation_hash, our_rhash);
	proto_to_sha256(o2->revocation_hash, their_rhash);

	assert(tal_count(cstate->a.htlcs) == 0);
	assert(tal_count(cstate->b.htlcs) == 0);
	
	/* If o2 sent anchor, it contains their commit sig. */
	if (o2->anch == OPEN_CHANNEL__ANCHOR_OFFER__WILL_CREATE_ANCHOR)
		sig = oa->commit_sig;

	if (num_updates)
		*num_updates = 0;
	while (*argv) {
		int64_t delta, amount;
		size_t n;
		bool received;
		Pkt *pkt;

		/* + marks messages sent by us, - for messages from them */
		if (strstarts(*argv, "+")) {
			received = false;
		} else if (strstarts(*argv, "-")) {
			received = true;
		} else
			errx(1, "%s does not start with +/-", *argv);

		pkt = any_pkt_from_file(*argv + 1);
		switch (pkt->pkt_case) {
		case PKT__PKT_OPEN_COMMIT_SIG:
			if (received)
				sig = pkt->open_commit_sig->sig;
			break;
		case PKT__PKT_UPDATE_ADD_HTLC:
			amount = pkt->update_add_htlc->amount_msat;
			if (received) {
				if (!funding_delta(is_funder(o2), oa->amount,
						   0, amount,
						   &cstate->b, &cstate->a))
					errx(1, "Impossible htlc %llu %s",
					     (long long)amount, *argv);
				add_htlc(&cstate->b, pkt->update_add_htlc,
					 *argv);
			} else {
				if (!funding_delta(is_funder(o1), oa->amount,
						   0, amount,
						   &cstate->a, &cstate->b))
					errx(1, "Impossible htlc %llu %s",
					     (long long)amount, *argv);
				add_htlc(&cstate->a, pkt->update_add_htlc,
					 *argv);
			}
				
			update_rhash(pkt->update_add_htlc->revocation_hash,
				     received, num_updates,
				     &old_our_rhash, &old_their_rhash,
				     our_rhash, their_rhash);
			break;

		case PKT__PKT_UPDATE_TIMEDOUT_HTLC:
			if (received) {
				n = find_htlc(&cstate->b,
					      pkt->update_timedout_htlc->r_hash);
				if (n == tal_count(cstate->b.htlcs))
					errx(1, "Unknown R hash in %s", *argv);
				amount = cstate->b.htlcs[n].msatoshis;
				if (!funding_delta(is_funder(o2), oa->amount,
						   0, -amount,
						   &cstate->b, &cstate->a))
					errx(1, "Impossible htlc %llu %s",
					     (long long)amount, *argv);
				remove_htlc(&cstate->b, n);
			} else {
				n = find_htlc(&cstate->a,
					      pkt->update_timedout_htlc->r_hash);
				if (n == tal_count(cstate->a.htlcs))
					errx(1, "Unknown R hash in %s", *argv);
				amount = cstate->a.htlcs[n].msatoshis;
				if (!funding_delta(is_funder(o1), oa->amount,
						   0, -amount,
						   &cstate->a, &cstate->b))
					errx(1, "Impossible htlc %llu %s",
					     (long long)amount, *argv);
				remove_htlc(&cstate->a, n);
			}
			update_rhash(pkt->update_timedout_htlc->revocation_hash,
				     received, num_updates,
				     &old_our_rhash, &old_their_rhash,
				     our_rhash, their_rhash);
			break;

		/* HTLC acceptor sends this to initiator. */
		case PKT__PKT_UPDATE_ROUTEFAIL_HTLC:
			if (received) {
				n = find_htlc(&cstate->a,
					      pkt->update_routefail_htlc->r_hash);
				if (n == tal_count(cstate->a.htlcs))
					errx(1, "Unknown R hash in %s", *argv);
				amount = cstate->a.htlcs[n].msatoshis;
				if (!funding_delta(is_funder(o1), oa->amount,
						   0, -amount,
						   &cstate->a, &cstate->b))
					errx(1, "Impossible htlc %llu %s",
					     (long long)amount, *argv);
				remove_htlc(&cstate->a, n);
			} else {
				n = find_htlc(&cstate->b,
					      pkt->update_routefail_htlc->r_hash);
				if (n == tal_count(cstate->b.htlcs))
					errx(1, "Unknown R hash in %s", *argv);
				amount = cstate->b.htlcs[n].msatoshis;
				if (!funding_delta(is_funder(o2), oa->amount,
						   0, -amount,
						   &cstate->b, &cstate->a))
					errx(1, "Impossible htlc %llu %s",
					     (long long)amount, *argv);
				remove_htlc(&cstate->b, n);
			}
			update_rhash(pkt->update_routefail_htlc->revocation_hash,
				     received, num_updates,
				     &old_our_rhash, &old_their_rhash,
				     our_rhash, their_rhash);
			break;
			
		case PKT__PKT_UPDATE_FULFILL_HTLC: {
			struct sha256 r_hash, r_val;
			Sha256Hash *rh;

			/* Get hash, to find the HTLC. */
			proto_to_sha256(pkt->update_fulfill_htlc->r, &r_val);
			sha256(&r_hash, &r_val, sizeof(r_val));
			rh = sha256_to_proto(ctx, &r_hash);

			if (received) {
				u64 b_before = cstate->b.pay_msat + cstate->b.fee_msat;
				/* HTLC was us->them, funds go to them. */
				n = find_htlc(&cstate->a, rh);
				if (n == tal_count(cstate->a.htlcs))
					errx(1, "Unknown R hash in %s", *argv);
				amount = cstate->a.htlcs[n].msatoshis;
				if (!funding_delta(is_funder(o1), oa->amount,
						   -amount, -amount,
						   &cstate->a, &cstate->b))
					errx(1, "Impossible htlc %llu %s",
					     (long long)amount, *argv);
				remove_htlc(&cstate->a, n);
				assert(cstate->b.pay_msat + cstate->b.fee_msat
				       == b_before + amount);
			} else {
				/* HTLC was them->us, funds go to us. */
				n = find_htlc(&cstate->b, rh);
				if (n == tal_count(cstate->b.htlcs))
					errx(1, "Unknown R hash in %s", *argv);
				amount = cstate->b.htlcs[n].msatoshis;
				if (!funding_delta(is_funder(o2), oa->amount,
						   -amount, -amount,
						   &cstate->b, &cstate->a))
					errx(1, "Impossible htlc %llu %s",
					     (long long)amount, *argv);
				remove_htlc(&cstate->b, n);
			}
			update_rhash(pkt->update_fulfill_htlc->revocation_hash,
				     received, num_updates,
				     &old_our_rhash, &old_their_rhash,
				     our_rhash, their_rhash);
			break;
		}

		case PKT__PKT_UPDATE:
			if (received)
				delta = -pkt->update->delta_msat;
			else
				delta = pkt->update->delta_msat;
			if (!funding_delta(is_funder(o1), oa->amount, delta, 0,
					   &cstate->a, &cstate->b))
				errx(1, "Impossible funding update %lli %s",
				     (long long)delta, *argv);

			update_rhash(pkt->update->revocation_hash,
				     received, num_updates,
				     &old_our_rhash, &old_their_rhash,
				     our_rhash, their_rhash);
			break;
		case PKT__PKT_UPDATE_ACCEPT:
			if (received)
				sig = pkt->update_accept->sig;

			/* Does not increase num_updates */
			update_rhash(pkt->update_accept->revocation_hash,
				     received, NULL,
				     &old_our_rhash, &old_their_rhash,
				     our_rhash, their_rhash);
			break;
		case PKT__PKT_UPDATE_SIGNATURE:
			if (received) {
				sig = pkt->update_signature->sig;
				check_preimage(pkt->update_signature
					       ->revocation_preimage,
					       &old_their_rhash, their_rhash,
					       *argv);
			} else {
				check_preimage(pkt->update_signature
					       ->revocation_preimage,
					       &old_our_rhash, our_rhash,
					       *argv);
			}
			break;
		case PKT__PKT_UPDATE_COMPLETE:
			if (received) {
				check_preimage(pkt->update_complete
					       ->revocation_preimage,
					       &old_their_rhash, their_rhash,
					       *argv);
			} else {
				check_preimage(pkt->update_complete
					       ->revocation_preimage,
					       &old_our_rhash, our_rhash,
					       *argv);
			}
			break;
		default:
			errx(1, "Unexpected packet type %u", pkt->pkt_case);
		}
		argv++;
	}

	if (their_commit_sig) {
		if (!sig)
			errx(1, "No commit signature message found");
		if (!proto_to_signature(sig, their_commit_sig))
			errx(1, "Invalid signature");
	}

	assert(htlcs_total(cstate->a.htlcs)
	       + cstate->a.pay_msat + cstate->a.fee_msat
	       + htlcs_total(cstate->b.htlcs)
	       + cstate->b.pay_msat + cstate->b.fee_msat
	       == oa->amount * 1000);

	return cstate;
}
