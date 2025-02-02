/*
 * Copyright (c) 2019 Robert Falkenberg.
 *
 * This file is part of FALCON 
 * (see https://github.com/falkenber9/falcon).
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as
 * published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * A copy of the GNU Affero General Public License can be found in
 * the LICENSE file in the top-level directory of this distribution
 * and at http://www.gnu.org/licenses/.
 */

#include <math.h>
#include "falcon/phy/falcon_ue/falcon_ue_dl.h"
#include "falcon/util/rnti_manager_c.h"

#define CNI_HISTOGRAM

#define RNTILIFELEN 2

//#define PRINT_SCAN_TIME

//const srslte_dci_format_t falcon_ue_all_formats[] = {
//  SRSLTE_DCI_FORMAT0,
//  SRSLTE_DCI_FORMAT1,
//  SRSLTE_DCI_FORMAT1A,
//  SRSLTE_DCI_FORMAT1C,
//  SRSLTE_DCI_FORMAT1B,
//  SRSLTE_DCI_FORMAT1D,
//  SRSLTE_DCI_FORMAT2,
//  SRSLTE_DCI_FORMAT2A,
//};
//const uint32_t nof_falcon_ue_all_formats = 8;

#define IMDEA_OWL_COMPAT
#ifndef IMDEA_OWL_COMPAT

const srslte_dci_format_t falcon_ue_all_formats[] = {
  SRSLTE_DCI_FORMAT0,
  SRSLTE_DCI_FORMAT1,
  SRSLTE_DCI_FORMAT1A,
  SRSLTE_DCI_FORMAT1C,
  SRSLTE_DCI_FORMAT2A,
};
const uint32_t nof_falcon_ue_all_formats = 5;

#else

const srslte_dci_format_t falcon_ue_all_formats[] = {
  SRSLTE_DCI_FORMAT0,
  SRSLTE_DCI_FORMAT1,
  SRSLTE_DCI_FORMAT1A,
  SRSLTE_DCI_FORMAT1B,
  SRSLTE_DCI_FORMAT1C,
  SRSLTE_DCI_FORMAT2,
  SRSLTE_DCI_FORMAT2A
};
const uint32_t nof_falcon_ue_all_formats = 7;

#endif





int falcon_ue_dl_init(falcon_ue_dl_t *q,
                      srslte_ue_dl_t *qq,
                      cf_t *in_buffer[SRSLTE_MAX_PORTS],
                      uint32_t max_prb,
                      uint32_t nof_rx_antennas,
                      const char* dci_file_name,
                      const char* stats_file_name)
{
  srslte_ue_dl_reset_rnti_list(q);

  // CNI contribution - init histogram
  q->rnti_histogram = calloc(nof_falcon_ue_all_formats, sizeof(rnti_histogram_t));
  for(int hst=0; hst<nof_falcon_ue_all_formats; hst++) {
    rnti_histogram_init(&q->rnti_histogram[hst]);
  }

  q->decoderthread = 0;

  q->rnti_manager = 0;
  if(dci_file_name != NULL && strlen(dci_file_name) > 0) {
    q->dci_file = fopen(dci_file_name, "w");
  }
  else {
    q->dci_file = stdout;
  }

  if(stats_file_name != NULL && strlen(stats_file_name) > 0) {
    q->stats_file = fopen(stats_file_name, "w");
  }
  else {
    q->stats_file = stdout;
  }

  q->colored_rb_map_dw = q->colored_rb_map_dw_bufA;
  q->colored_rb_map_up = q->colored_rb_map_up_bufA;
  q->colored_rb_map_dw_last = q->colored_rb_map_dw_bufB;
  q->colored_rb_map_up_last = q->colored_rb_map_up_bufB;

  q->stats.nof_cce = 0;
  q->stats.nof_decoded_locations = 0;
  q->stats.nof_missed_cce = 0;
  q->stats.nof_subframes = 0;
  q->stats.nof_subframe_collisions_dw = 0;
  q->stats.nof_subframe_collisions_up = 0;
  q->stats.time_blindsearch.tv_sec = 0;
  q->stats.time_blindsearch.tv_usec = 0;
  q->q = qq;
  return srslte_ue_dl_init(q->q, in_buffer, max_prb, nof_rx_antennas);
}

void falcon_ue_dl_free(falcon_ue_dl_t *q) {
  if(q) {
    srslte_ue_dl_free(q->q);

    if(q->dci_file != stdout) {
      fclose(q->dci_file);
    }
    if(q->stats_file != stdout) {
      fclose(q->stats_file);
    }

    for(int hst=0; hst<nof_falcon_ue_all_formats; hst++) {
      rnti_histogram_free(&q->rnti_histogram[hst]);
    }
    free(q->rnti_histogram);
    q->rnti_histogram = 0;
  }
}

dci_candidate_t* falcon_alloc_candidates(uint32_t nof_candidates) {
  dci_candidate_t* result = calloc(nof_candidates, sizeof(dci_candidate_t));
  return result;
}

void falcon_free_candidates(dci_candidate_t* candidates) {
  free(candidates);
}

void srslte_ue_dl_reset_rnti_list(falcon_ue_dl_t *q) {
  bzero(q->rnti_list, 65536*sizeof(uint8_t));
  bzero(q->rnti_cnt, 65536*sizeof(uint8_t));
  for (int i = 1; i <= 10; i++) {
      q->rnti_list[i] = 1;
    }
  q->rnti_list[65534] = 1;
  q->rnti_list[65535] = 1;
}

void srslte_ue_dl_reset_rnti_user(falcon_ue_dl_t *q, uint16_t user) {
  q->rnti_list[user] = RNTILIFELEN;
  q->rnti_cnt[user]++;
}

void srslte_ue_dl_reset_rnti_user_to(falcon_ue_dl_t *q, uint16_t user, uint16_t val) {
  q->rnti_list[user] = val;
}

void srslte_ue_dl_update_rnti_list(falcon_ue_dl_t *q) {
  for (int i = 10; i < 65533; i++) {
      q->rnti_list[i] = (q->rnti_list[i]>0) ? q->rnti_list[i]-1 : 0;
    }
}

int rnti_in_list(falcon_ue_dl_t *q, uint16_t check) {
  return (q->rnti_list[check]);
}

void srslte_ue_dl_stats_print(falcon_ue_dl_t *q, FILE* f) {
  dci_blind_search_stats_t* s = &q->stats;
//  printf("Stats: nof_loc %d; nof_cce %d; nof_missed %d; sf %d; collisions dw %d ul %d; time %ld.%ld\n",
//         s->nof_decoded_locations,
//         s->nof_cce,
//         s->nof_missed_cce,
//         s->nof_subframes,
//         s->nof_subframe_collisions_dw,
//         s->nof_subframe_collisions_up,
//         s->time_blindsearch.tv_sec,
//         s->time_blindsearch.tv_usec);
  fprintf(f, "nof_decoded_locations, nof_cce, nof_missed_cce, nof_subframes, nof_subframe_collisions_dw, nof_subframe_collisions_up, time\n");
  fprintf(f, "%d, %d, %d, %d, %d, %d, %ld.%06ld\n",
          s->nof_decoded_locations,
          s->nof_cce,
          s->nof_missed_cce,
          s->nof_subframes,
          s->nof_subframe_collisions_dw,
          s->nof_subframe_collisions_up,
          s->time_blindsearch.tv_sec,
          s->time_blindsearch.tv_usec);

}

int srslte_ue_dl_find_dci_cc(falcon_ue_dl_t *q, srslte_dci_msg_t *dci_msg, uint32_t cfi, uint32_t sf_idx, uint32_t sfn, bool reencoding_only)
/* IMDEA contribution: DCI power analysis */
{
  falcon_dci_location_t locations[MAX_CANDIDATES_BLIND];
  uint32_t nof_locations;
  uint32_t nof_formats;
  srslte_ra_dl_dci_t dl_dci_unpacked;
  srslte_ra_ul_dci_t ul_dci_unpacked;
  srslte_ra_dl_grant_t dl_grant;
  srslte_ra_ul_grant_t ul_grant;
  srslte_dci_format_t const *formats = NULL;
  uint16_t crc_rem = 0;
  int lprob[MAX_CANDIDATES_BLIND];
  float power = 0;
  int ret = 0;
  q->collision_dw = false;
  q->collision_up = false;
  q->stats.nof_cce += q->q->pdcch.nof_cce[0] + q->q->pdcch.nof_cce[1] + q->q->pdcch.nof_cce[2];

#ifdef CNI_TIMESTAMP
  struct timeval timestamp;
  gettimeofday(&timestamp, NULL);
#endif

  /* Generate PDCCH candidates allowing all possible control channel locations in which something is sensed */
  nof_locations = srslte_pdcch_ue_locations_all(&q->q->pdcch, locations, MAX_CANDIDATES_BLIND, sf_idx, cfi);
  formats = falcon_ue_all_formats;
  /* Define power only to have a check of all possible size of DCI messages */
#ifdef POWER_ONLY
  nof_formats = 1;
#else
  nof_formats = nof_falcon_ue_all_formats;
#endif
  q->q->current_rnti = 0xffff;
  bzero(q->colored_rb_map_dw, sizeof(((falcon_ue_dl_t *)0)->colored_rb_map_dw_bufA));
  bzero(q->colored_rb_map_up, sizeof(((falcon_ue_dl_t *)0)->colored_rb_map_up_bufA));
  q->totRBup = 0;
  q->totRBdw = 0;
  q->totBWup = 0;
  q->totBWdw = 0;

  for (uint32_t i=0;i<nof_locations;i++) {
      /* Avoid to check any location whose power is lower than a given threshold and those that have already been checked */
      if (locations[i].power < PWR_THR || locations[i].checked) {
          continue;
        }
      for (uint32_t f=0;f<nof_formats;f++) {
#ifdef POWER_ONLY
        /* tries all possible DCI sizes and returns the one with the highest re-encoding prob (or the first with a known C-RNTI) */
        lprob[i] = (int)(round((double)srslte_pdcch_decode_msg_check_power(&q->q->pdcch, cfi, dci_msg, &locations[i], formats[f], &crc_rem, q->rnti_list)));
#else
        /* tries to decode a DCI message of a given format */
        lprob[i] = (int)(round((double)srslte_pdcch_decode_msg_check(&q->q->pdcch, cfi, dci_msg, &locations[i], formats[f], &crc_rem, q->rnti_list)));
        q->stats.nof_decoded_locations++;
#endif
      //printf("Cand. %d\tProb. %d\n", crc_rem, lprob[i]);
	  /* checks whether the first bit is coherent with the format */
	  if ((formats[f]==SRSLTE_DCI_FORMAT0 && dci_msg->data[0]==1) || (formats[f]==SRSLTE_DCI_FORMAT1A && dci_msg->data[0]==0)) continue;
	  /* checks whether P/SI/RA-RNTI addresses are used with formats other than 0 and 1a */
	  if (!(formats[f]==SRSLTE_DCI_FORMAT0 || formats[f]==SRSLTE_DCI_FORMAT1A) && (crc_rem <= 0x000a || crc_rem > 0xfff3)) continue;
	  /* checks whether the C-RNTI found is coherent with a scheduling in that location */
	  if (!srslte_pdcch_ue_locations_check(&q->q->pdcch, sf_idx, cfi, crc_rem, locations[i].ncce)) continue;
	  if (crc_rem == 180) {
	      printf("HALT!\n");
	    }
	  if (rnti_in_list(q, crc_rem) || lprob[i] >= PROB_THR) {
	      //printf("t=%d.%d (%d (%d)), ret %d, crnti 0x%x\n", sfn, sf_idx, locations[i].ncce, locations[i].L, lprob[i], crc_rem);
	      /* print the DCI information */
	      /* TODO save this information somewhere to be reused by other modules */
	      if (srslte_dci_msg_to_trace(dci_msg, crc_rem, q->q->cell.nof_prb, q->q->cell.nof_ports,
					  &dl_dci_unpacked, &ul_dci_unpacked, &dl_grant, &ul_grant, sf_idx, sfn, (uint32_t)lprob[i],
                      locations[i].ncce, locations[i].L, formats[f], cfi, power, q->dci_file)) {
		  continue;
		  //fprintf(stderr,"1 Error unpacking DCI\n");
		}
          if(!reencoding_only) {
            /* add the C-RNTI to the white list */
            srslte_ue_dl_reset_rnti_user(q, crc_rem);
          }
	      ret++;
	      /* checks whether the DCI carries an RA-RNTI. If so marks the current_rnti as the RA-RNTI */
	      if (crc_rem > 0x0000 && crc_rem <= 0x000a && formats[f]!=SRSLTE_DCI_FORMAT0) q->q->current_rnti = crc_rem;
	      /* save temp information for the graphic version */
	      if (dl_grant.mcs[0].tbs>0) {
		  /* merge total RB map for RB allocation overview */
		  uint16_t color = crc_rem;
		  color = (((color & 0xFF00) >> 8) | ((color & 0x00FF) << 8));
		  color = (((color & 0xF0F0) >> 4) | ((color & 0x0F0F) << 4));
		  color = (((color & 0xCCCC) >> 2) | ((color & 0x3333) << 2));
		  color = (((color & 0xAAAA) >> 1) | ((color & 0x5555) << 1));
		  for(uint32_t rb_idx = 0; rb_idx < q->q->cell.nof_prb; rb_idx++) {
		      if(dl_grant.prb_idx[0][rb_idx] == true) {
			  if(q->colored_rb_map_dw[rb_idx] != 0) {
			      q->collision_dw = true;
			    }
			  q->colored_rb_map_dw[rb_idx] = color;
			}
		    }

		  q->totRBdw += dl_grant.nof_prb;
		  q->totBWdw += (uint32_t)(dl_grant.mcs[0].tbs + dl_grant.mcs[1].tbs);
		  if (q->totRBdw > q->q->cell.nof_prb) q->totBWdw = q->q->cell.nof_prb;
		}
	      if (ul_grant.mcs.tbs>0) {
		  /* merge total RB map for RB allocation overview */
		  uint16_t color = crc_rem;
		  color = (((color & 0xFF00) >> 8) | ((color & 0x00FF) << 8));
		  color = (((color & 0xF0F0) >> 4) | ((color & 0x0F0F) << 4));
		  color = (((color & 0xCCCC) >> 2) | ((color & 0x3333) << 2));
		  color = (((color & 0xAAAA) >> 1) | ((color & 0x5555) << 1));
		  for(uint32_t rb_idx = 0; rb_idx < ul_grant.L_prb; rb_idx++) {
		      if(q->colored_rb_map_up[ul_grant.n_prb[0] + rb_idx] != 0) {
			  q->collision_up = true;
			}
		      q->colored_rb_map_up[ul_grant.n_prb[0] + rb_idx] = color;
		    }

		  q->totRBup += ul_grant.L_prb;
		  q->totBWup += (uint32_t) ul_grant.mcs.tbs;
		  if (q->totRBup > q->q->cell.nof_prb) q->totBWup = q->q->cell.nof_prb;
		}
	      /* marks all locations overlapped by and overlapping the current DCI message as already checked */
	      for (uint32_t j=i;j<nof_locations;j++) {
		  if (locations[j].ncce >= locations[i].ncce && locations[j].ncce < (locations[i].ncce + (1 << locations[i].L))) {
		      locations[j].checked = 1;
		      DEBUG("skipping location %d with agg %d\n",locations[j].ncce,locations[j].L);
		    }
		}
	      for (uint32_t j=0;j<i;j++) {
		  if (locations[i].ncce >= locations[j].ncce && locations[i].ncce < (locations[j].ncce + (1 << locations[j].L))) {
		      locations[j].checked = 1;
		    }
		}

	    }
	} // end of format cycle
    } // end of location cycle
  /* The following print on stderr all locations that possibly contain a DCI and could not be decoded */

  if(ret == 0) {
      //printf("Empty Subframe!\n");
      q->colored_rb_map_dw[0] = 60000.0;
      q->colored_rb_map_up[0] = 60000.0;
    }
  //swap colored rb map buffers
  float* tmp_dw = q->colored_rb_map_dw_last;
  float* tmp_up = q->colored_rb_map_up_last;
  q->colored_rb_map_dw_last = q->colored_rb_map_dw;
  q->colored_rb_map_up_last = q->colored_rb_map_up;
  q->colored_rb_map_dw = tmp_dw;
  q->colored_rb_map_up = tmp_up;

  if(q->collision_dw) {
    q->stats.nof_subframe_collisions_dw++;
    //fprintf(stderr, "DL collision detected\n");
    printf("DL collision detected\n");
  }
  if(q->collision_up) {
    q->stats.nof_subframe_collisions_up++;
    fprintf(stderr, "UL collision detected\n");
  }
  uint32_t missed = srslte_pdcch_nof_missed_cce_compat(locations, nof_locations, cfi);
  if(missed > 0) {
    fprintf(stderr, "Missed CCEs in SFN %d.%d: %d\n", sfn, sf_idx, missed);
  }
  q->stats.nof_missed_cce += missed;

#ifdef LOG_ERRORS
  for (int i=0;i<nof_locations;i++) {
      if (locations[i].power >= PWR_THR && !locations[i].checked) {
        fprintf(stderr,"%d\t%d\t%d\t%d\t%d\n",sfn,sf_idx,locations[i].ncce,locations[i].L,cfi);
      }
    }
#endif
  return ret;
}

/* CNI contribution: Recursive Histogram-Based DCI filtering */
int srslte_ue_dl_inspect_dci_location_recursively(falcon_ue_dl_t *q,
                                                  srslte_dci_msg_t *dci_msg,
                                                  uint32_t cfi,
                                                  uint32_t sf_idx,
                                                  uint32_t sfn,
                                                  falcon_cce_to_dci_location_map_t *cce_map,
                                                  falcon_dci_location_t *location_list,
                                                  uint32_t ncce,
                                                  uint32_t L,
                                                  srslte_dci_format_t const *formats,
                                                  uint32_t nof_formats,
                                                  uint32_t enable_shortcut,
                                                  const dci_candidate_t parent_cand[]) {
  //uint16_t rnti_cand[nof_formats];
  //srslte_dci_msg_t dci_msg_cand[nof_formats];
  int hist_max_format_idx = -1;
  unsigned int hist_max_format_value = 0;
  unsigned int nof_cand_above_threshold = 0;

  srslte_ra_dl_dci_t dl_dci_unpacked;
  srslte_ra_ul_dci_t ul_dci_unpacked;
  srslte_ra_dl_grant_t dl_grant;
  srslte_ra_ul_grant_t ul_grant;

  dci_candidate_t* cand = falcon_alloc_candidates(nof_formats);

  //struct timeval t[3];

  struct timeval timestamp;
  gettimeofday(&timestamp, NULL);

  if (cce_map[ncce].location[L] && !cce_map[ncce].location[L]->checked && cce_map[ncce].location[L]->sufficient_power) {
    // Decode candidate for each format (CRC and DCI)
    for(uint32_t format_idx=0; format_idx<nof_formats; format_idx++) {
      //gettimeofday(&t[1], NULL);
      int result = srslte_pdcch_decode_msg_limit_avg_llr_power(&q->q->pdcch, &cand[format_idx].dci_msg, cce_map[ncce].location[L], formats[format_idx], cfi, &cand[format_idx].rnti, 0);
      q->stats.nof_decoded_locations++;
#ifdef PRINT_ALL_CANDIDATES
      printf("Cand. %d (sfn %d.%d, ncce %d, L %d, f_idx %d)\n", &cand[format_idx].rnti, sfn, sf_idx, ncce, L, format_idx);
#endif
      if(result != SRSLTE_SUCCESS) {
        ERROR("Error calling srslte_pdcch_decode_msg_limit_avg_llr_power\n");
      }
      if(formats[format_idx] != cand[format_idx].dci_msg.format) {
        //format 0/1A format mismatch
        INFO("Dropped DCI cand. %d (format_idx %d) L%d ncce %d (format 0/1A mismatch)\n", cand[format_idx].rnti, format_idx, L, ncce);
        cand[format_idx].rnti = FALCON_ILLEGAL_RNTI;
        continue;
      }
      //gettimeofday(&t[2], NULL);
      //get_time_interval(t);
      //printf("Decoding time: %d us (sfn %d, ncce %d, L %d)\n", t[0].tv_usec, sfn, ncce, L);

      // Filter 1a: Disallowed RNTI values for format 1C
      if ((formats[format_idx]==SRSLTE_DCI_FORMAT1C) && (cand[format_idx].rnti > SRSLTE_RARNTI_END) && (cand[format_idx].rnti < SRSLTE_PRNTI)) {
        //DEBUG("Dropped DCI 1C cand. by illegal RNTI: 0x%x\n", cand[format_idx].rnti);
        INFO("Dropped DCI cand. %d (format_idx %d) L%d ncce %d (RNTI not allowed in format1C)\n", cand[format_idx].rnti, format_idx, L, ncce);
        cand[format_idx].rnti = FALCON_ILLEGAL_RNTI;
        continue;
      }
      // Filter 1a-RA: Disallowed formats for RA-RNTI
      if ((cand[format_idx].rnti > SRSLTE_RARNTI_START) &&
          (cand[format_idx].rnti < SRSLTE_RARNTI_END)) {
        if(formats[format_idx]==SRSLTE_DCI_FORMAT1A) {
          INFO("Found RA-RNTI: 0x%x\n", cand[format_idx].rnti);
          q->q->current_rnti = cand[format_idx].rnti;
        }
        else {
          //DEBUG("Dropped DCI cand.: RA-RNTI only with format 1A: 0x%x\n", cand[format_idx].rnti);
          INFO("Dropped DCI cand. %d (format_idx %d) L%d ncce %d (RA-RNTI only format 1A)\n", cand[format_idx].rnti, format_idx, L, ncce);
          cand[format_idx].rnti = FALCON_ILLEGAL_RNTI;
          continue;
        }
      }
      // Shortcut: Decoding at smaller aggregation level still results in the same RNTI as parent.
      // This is a very strong indicator, that the parent DCI is valid.
      if(enable_shortcut &&
         parent_cand != NULL &&
         parent_cand[format_idx].rnti == cand[format_idx].rnti &&
         !rnti_manager_is_forbidden(q->rnti_manager, cand[format_idx].rnti, format_idx))
      {
        INFO("RNTI matches to parent DCI cand. %d (format_idx %d), this: L%d ncce %d\n", cand[format_idx].rnti, format_idx, L, ncce);
        falcon_free_candidates(cand);
        return -((int)format_idx+1);
      }

      // Filter 1b: Illegal DCI positions
      cand[format_idx].search_space_match_result = srslte_pdcch_validate_location(srslte_pdcch_nof_cce(&q->q->pdcch, cfi), ncce, L, sf_idx, cand[format_idx].rnti);
      if (cand[format_idx].search_space_match_result == 0) {
        //DEBUG("Dropped DCI cand. by illegal position: 0x%x\n", cand[format_idx].rnti);
        INFO("Dropped DCI cand. %d (format_idx %d) L%d ncce %d (illegal position)\n", cand[format_idx].rnti, format_idx, L, ncce);
        cand[format_idx].rnti = FALCON_ILLEGAL_RNTI;
        continue;
      }

#ifndef __NEW_HISTOGRAM__
      unsigned int occurence = rnti_histogram_get_occurence(&q->rnti_histogram[format_idx], cand[format_idx].rnti);
      // Online max-search
      if(occurence > RNTI_HISTOGRAM_THRESHOLD) {

#ifdef __DISABLED_FOR_TESTING__
        // Filter 2a (part2): Matches DCI-Format to recent DCI-Format for this RNTI
        if(cand[format_idx].dci_msg.format != SRSLTE_DCI_FORMAT0 &&
           cand[format_idx].dci_msg.format != q->rnti_format[cand[format_idx].rnti]) {
          INFO("Dropped DCI cand. by mismatching format: 0x%x\n", cand[format_idx].rnti);
          cand[format_idx].rnti = FALCON_ILLEGAL_RNTI;
          continue;
        }
#endif

        nof_cand_above_threshold++;
        if(occurence > hist_max_format_value) {
          hist_max_format_value = occurence;
          hist_max_format_idx = (int)format_idx;
        }
      }
#else
      if(rnti_manager_validate_and_refresh(q->rnti_manager, cand[format_idx].rnti, format_idx)) {
        nof_cand_above_threshold++;
        hist_max_format_idx = (int)format_idx;
        hist_max_format_value = rnti_manager_getFrequency(q->rnti_manager, cand[format_idx].rnti, format_idx);
      }
#endif
    }

    // if multiple active candidates found, select the most frequent
    if(nof_cand_above_threshold > 1) {
      INFO("Found %d matching candidates in different formats for same CCEs:\n", nof_cand_above_threshold);
      hist_max_format_idx = -1;
      uint32_t hist_max = 0;
      uint32_t hist = 0;
      for(uint32_t format_idx=0; format_idx<nof_formats; format_idx++) {
        if(cand[format_idx].rnti != FALCON_ILLEGAL_RNTI) {
          hist = rnti_manager_getFrequency(q->rnti_manager, cand[format_idx].rnti, format_idx);
          INFO("\t DCI cand. %d (format_idx %d) L%d ncce %d freq %d\n", cand[format_idx].rnti, format_idx, L, ncce, hist);
          if(hist > hist_max) {
            hist_max = hist;
            hist_max_format_idx = (int)format_idx;
            hist_max_format_value = rnti_manager_getFrequency(q->rnti_manager, cand[format_idx].rnti, format_idx);
          }
        }
      }
      if(hist_max_format_idx == -1) {
        INFO("\t Multiple valid DCI candidates for same CCE location, but none appear in histogram. Skipping all\n");
        nof_cand_above_threshold = 0;
      }
      else {
        INFO("\t Selected DCI cand. %d (format_idx %d)\n", cand[hist_max_format_idx].rnti, hist_max_format_idx);
      }
    }

    cce_map[ncce].location[L]->checked = 1;

    int disambiguation_count = 0;
#ifdef ENABLE_DCI_DISAMBIGUATION
    // check, if the location of the preferred DCI is ambiguous with L-1
    // to prevent overshadowing of neighbouring DCI with L-1 in the right half
    if(nof_cand_above_threshold > 0 &&
       cand[hist_max_format_idx].search_space_match_result == SEARCH_SPACE_MATCH_RESULT_AMBIGUOUS ) {

      //descend only right half; pass NULL as parent_rnti_cands
      disambiguation_count = srslte_ue_dl_inspect_dci_location_recursively(q, dci_msg, cfi, sf_idx, sfn, cce_map, location_list, ncce + (1 << (L-1)), L-1, formats, nof_formats, 0, NULL);
      if(disambiguation_count > 0) {
        INFO("Disambiguation discovered %d additional DCI\n", disambiguation_count);
      }
    }
    else
#endif
    // if no candidates were found, descend covered CCEs recursively
    if(nof_cand_above_threshold == 0) {
      // found nothing - recursion, if applicable
      int recursion_result = 0;
      if(L > 0) {
        //descend left half; pass parent_rnti_cands for shortcuts...
        recursion_result += srslte_ue_dl_inspect_dci_location_recursively(q, dci_msg, cfi, sf_idx, sfn, cce_map, location_list, ncce, L-1, formats, nof_formats, enable_shortcut, cand);
        if(recursion_result < 0) {
          //shortcut taken, activate RNTI
          INFO("Shortcut detected: RNTI: %d (format_idx %d)!\n", cand[-recursion_result - 1].rnti, -recursion_result - 1);
          hist_max_format_idx = -recursion_result - 1;
          hist_max_format_value = rnti_manager_getFrequency(q->rnti_manager, cand[hist_max_format_idx].rnti, (uint32_t)hist_max_format_idx);
          nof_cand_above_threshold = 1;
          rnti_manager_activate_and_refresh(q->rnti_manager, cand[hist_max_format_idx].rnti, (uint32_t)hist_max_format_idx, RM_ACT_SHORTCUT);
        }
        else {
          //descend right half; pass NULL as parent_rnti_cands
          recursion_result += srslte_ue_dl_inspect_dci_location_recursively(q, dci_msg, cfi, sf_idx, sfn, cce_map, location_list, ncce + (1 << (L-1)), L-1, formats, nof_formats, enable_shortcut, NULL);
        }
      }

      // evaluate recursion:
      // recursion_result <  0: shortcut taken
      // recursion_result == 0: nothing found, add all cand. to histogram
      // recursion_result >  0: return this value as number of findings
      if(recursion_result == 0) {
        for(uint32_t format_idx=0; format_idx<nof_formats; format_idx++) {
          if(cand[format_idx].rnti != FALCON_ILLEGAL_RNTI) {
#ifndef __NEW_HISTOGRAM__
            rnti_histogram_add_rnti(&q->rnti_histogram[0], cand[format_idx].rnti);
#else
            rnti_manager_add_candidate(q->rnti_manager, cand[format_idx].rnti, format_idx);
            INFO("Dropped DCI cand. %d (format_idx %d) L%d ncce %d (spurious/infrequent), add to histogram\n", cand[format_idx].rnti, format_idx, L, ncce);
#endif

#ifdef __DISABLED_FOR_TESTING__
            // Filter 2a (part1): Remember DCI-Format of this RNTI
            if(cand[format_idx].dci_msg.format != SRSLTE_DCI_FORMAT0) {
              q->rnti_format[cand[format_idx].rnti] = cand[format_idx].dci_msg.format;
            }
#endif
          }
        }
        falcon_free_candidates(cand);
        return 0;
      }
      else if(recursion_result > 0) {
        falcon_free_candidates(cand);
        return recursion_result;
      }
      //else if(recursion_result < 0) { do not return }
    } // recursion

    // Found matching candidate (either by histogram or by shortcut in recursion)
    if(nof_cand_above_threshold > 0) {
      // found candidate - no recursion
      cce_map[ncce].location[L]->used = 1;

      // mark all other locations which overlap this location as checked
      for(uint32_t cce_idx=ncce; cce_idx < ncce+(1 << L); cce_idx++) {
        if (cce_map[cce_idx].location[0]) cce_map[cce_idx].location[0]->checked = 1;
        if (cce_map[cce_idx].location[1]) cce_map[cce_idx].location[1]->checked = 1;
        if (cce_map[cce_idx].location[2]) cce_map[cce_idx].location[2]->checked = 1;
        if (cce_map[cce_idx].location[3]) cce_map[cce_idx].location[3]->checked = 1;
      }

      // consider only the most frequent dci candidate; ignore others
#ifndef __NEW_HISTOGRAM__
      rnti_histogram_add_rnti(&q->rnti_histogram[0], cand[hist_max_format_idx].rnti);
#else
      rnti_manager_add_candidate(q->rnti_manager, cand[hist_max_format_idx].rnti, (uint32_t)hist_max_format_idx);
#endif

      // process the accepted DCI
      srslte_dci_msg_to_trace_timestamp(&cand[hist_max_format_idx].dci_msg, cand[hist_max_format_idx].rnti, q->q->cell.nof_prb, q->q->cell.nof_ports,
                                        &dl_dci_unpacked, &ul_dci_unpacked, &dl_grant, &ul_grant, sf_idx, sfn, hist_max_format_value,
                                        ncce, L, cand[hist_max_format_idx].dci_msg.format, cfi, 0, timestamp, hist_max_format_value, q->dci_file);

      srslte_dci_msg_to_trace_toTop(&cand[hist_max_format_idx].dci_msg, cand[hist_max_format_idx].rnti, q->q->cell.nof_prb, q->q->cell.nof_ports,
                                    &dl_dci_unpacked, &ul_dci_unpacked, &dl_grant, &ul_grant, sf_idx, sfn, hist_max_format_value,
                                    ncce, L, cand[hist_max_format_idx].dci_msg.format, cfi, 0, timestamp, hist_max_format_value, q->dci_file,q->decoderthread);

      // ####################################################################################################################################################

      // check upload/download
      if (dl_grant.mcs[0].tbs>0) {
        /* merge total RB map for RB allocation overview */
        uint16_t color = cand[hist_max_format_idx].rnti;
        color = (((color & 0xFF00) >> 8) | ((color & 0x00FF) << 8));
        color = (((color & 0xF0F0) >> 4) | ((color & 0x0F0F) << 4));
        color = (((color & 0xCCCC) >> 2) | ((color & 0x3333) << 2));
        color = (((color & 0xAAAA) >> 1) | ((color & 0x5555) << 1));
        color = (color >> 1) + 16000;
        for(uint32_t rb_idx = 0; rb_idx < q->q->cell.nof_prb; rb_idx++) {
          if(dl_grant.prb_idx[0][rb_idx] == true) {
            if(q->colored_rb_map_dw[rb_idx] != 0) {
              q->collision_dw = true;
            }
            q->colored_rb_map_dw[rb_idx] = color;
          }
        }

        q->totRBdw += dl_grant.nof_prb;
        q->totBWdw += (uint32_t)(dl_grant.mcs[0].tbs + dl_grant.mcs[1].tbs);
        if (q->totRBdw > q->q->cell.nof_prb) q->totBWdw = q->q->cell.nof_prb;
      }
      if (ul_grant.mcs.tbs>0) {
        /* merge total RB map for RB allocation overview */
        uint16_t color = cand[hist_max_format_idx].rnti;
        color = (((color & 0xFF00) >> 8) | ((color & 0x00FF) << 8));
        color = (((color & 0xF0F0) >> 4) | ((color & 0x0F0F) << 4));
        color = (((color & 0xCCCC) >> 2) | ((color & 0x3333) << 2));
        color = (((color & 0xAAAA) >> 1) | ((color & 0x5555) << 1));
        color = (color >> 1) + 16000;
        for(uint32_t rb_idx = 0; rb_idx < ul_grant.L_prb; rb_idx++) {
          if(q->colored_rb_map_up[ul_grant.n_prb[0] + rb_idx] != 0) {
            q->collision_up = true;
          }
          q->colored_rb_map_up[ul_grant.n_prb[0] + rb_idx] = color;
        }

        q->totRBup += ul_grant.L_prb;
        q->totBWup +=  (uint32_t) ul_grant.mcs.tbs;
        if (q->totRBup > q->q->cell.nof_prb) q->totBWup = q->q->cell.nof_prb;
      }
      falcon_free_candidates(cand);
      return 1 + disambiguation_count;
    }
    else {
      // this should never happen
      ERROR("nof_cand_above_threshold <= 0 but this was not caught earlier.\n");
    }
  }
  falcon_free_candidates(cand);
  return 0;
}

int srslte_ue_dl_recursive_blind_dci_search(falcon_ue_dl_t *q, srslte_dci_msg_t *dci_msg, uint32_t cfi, uint32_t sf_idx, uint32_t sfn)
/* CNI contribution: Recursive Histogram-Based DCI filtering */
{
  falcon_dci_location_t locations[MAX_CANDIDATES_BLIND];
  falcon_cce_to_dci_location_map_t cce_map[MAX_NUM_OF_CCE] = {{{0}, 0}};
  uint32_t nof_locations;
  uint32_t nof_formats;
  srslte_dci_format_t const *formats = NULL;
  int ret = 0;
  q->collision_dw = false;
  q->collision_up = false;
  q->stats.nof_cce += q->q->pdcch.nof_cce[0] + q->q->pdcch.nof_cce[1] + q->q->pdcch.nof_cce[2];

  //struct timeval t[3];

  /* Generate PDCCH candidates and a lookup map for cce -> parent dci */
  nof_locations = srslte_pdcch_ue_locations_all_map(&q->q->pdcch, locations, MAX_CANDIDATES_BLIND, cce_map, MAX_NUM_OF_CCE, sf_idx, cfi);

#if 0   //SISO
  formats = ue_siso_formats;
  nof_formats = nof_ue_siso_formats;
#endif

#if 1   //ALL Formats
  formats = falcon_ue_all_formats;
  nof_formats = nof_falcon_ue_all_formats;
#endif

  /* Calculate power levels on each cce*/
  srslte_pdcch_cce_avg_llr_power(&q->q->pdcch, cfi, cce_map, MAX_NUM_OF_CCE);

  q->q->current_rnti = 0xffff;
  bzero(q->colored_rb_map_dw, sizeof(((falcon_ue_dl_t *)0)->colored_rb_map_dw_bufA));
  bzero(q->colored_rb_map_up, sizeof(((falcon_ue_dl_t *)0)->colored_rb_map_up_bufA));
  q->totRBup = 0;
  q->totRBdw = 0;
  q->totBWup = 0;
  q->totBWdw = 0;

  //uint32_t L = 3;    // Aggregation level
  // inspect all locations at this aggregation level recursively
  for(unsigned int location_idx=0; location_idx < nof_locations; location_idx++) {
    ret += srslte_ue_dl_inspect_dci_location_recursively(q, dci_msg, cfi, sf_idx, sfn, cce_map, locations, locations[location_idx].ncce, locations[location_idx].L, formats, nof_formats, 1, NULL);
  }

  //    if(ret == 0) {
  //        //printf("Empty Subframe!\n");
  //        q->colored_rb_map_dw[15] = 60000.0;
  //        q->colored_rb_map_up[15] = 60000.0;
  //    }

  if(sfn == 1109 && sf_idx == 5) {

      q->colored_rb_map_dw[15] = 60000.0;
      q->colored_rb_map_dw[20] = 60000.0;
      q->colored_rb_map_dw[25] = 60000.0;
      q->colored_rb_map_dw[30] = 60000.0;
  }

  //swap colored rb map buffers
  float* tmp_dw = q->colored_rb_map_dw_last;
  float* tmp_up = q->colored_rb_map_up_last;
  q->colored_rb_map_dw_last = q->colored_rb_map_dw;
  q->colored_rb_map_up_last = q->colored_rb_map_up;
  q->colored_rb_map_dw = tmp_dw;
  q->colored_rb_map_up = tmp_up;

  if(q->collision_dw) {
    q->stats.nof_subframe_collisions_dw++;
    INFO("DL collision detected\n");
  }
  if(q->collision_up) {
    q->stats.nof_subframe_collisions_up++;
    INFO("UL collision detected\n");
  }
  uint32_t missed = srslte_pdcch_nof_missed_cce(&q->q->pdcch, cfi, cce_map, MAX_NUM_OF_CCE);
  if(missed > 0) {
    INFO("Missed CCEs in SFN %d.%d: %d\n", sfn, sf_idx, missed);
  }
  q->stats.nof_missed_cce += missed;

  rnti_manager_step_time(q->rnti_manager);

  return ret;
}

int srslte_ue_dl_find_dci_histogram(falcon_ue_dl_t *q, srslte_dci_msg_t *dci_msg, uint32_t cfi, uint32_t sf_idx, uint32_t sfn)
/* CNI contribution: Histogram-Based DCI filtering */
{
  falcon_dci_location_t locations[MAX_CANDIDATES_BLIND];
  falcon_cce_to_dci_location_map_t cce_map[MAX_NUM_OF_CCE] = {{{0}, 0}};
  uint32_t nof_locations;
  uint32_t nof_formats;
  srslte_ra_dl_dci_t dl_dci_unpacked;
  srslte_ra_ul_dci_t ul_dci_unpacked;
  srslte_ra_dl_grant_t dl_grant;
  srslte_ra_ul_grant_t ul_grant;
  srslte_dci_format_t const *formats = NULL;
  uint16_t crc_rem = 0;
  //  bool to_check[MAX_CANDIDATES_BLIND];
  //  int lprob[MAX_CANDIDATES_BLIND];
  float power = 0;
  int ret = 0;
  q->collision_dw = false;
  q->collision_up = false;
  q->stats.nof_cce += q->q->pdcch.nof_cce[0] + q->q->pdcch.nof_cce[1] + q->q->pdcch.nof_cce[2];

  struct timeval t[3];

#ifdef CNI_TIMESTAMP
  struct timeval timestamp;
  gettimeofday(&timestamp, NULL);
#endif

  if(sfn == 50 && sf_idx == 5) {   /* only for a quick breakpoint */
      gettimeofday(&timestamp, NULL);
    }

  /* Generate PDCCH candidates and a lookup map for cce -> parent dci */
  nof_locations = srslte_pdcch_ue_locations_all_map(&q->q->pdcch, locations, MAX_CANDIDATES_BLIND, cce_map, MAX_NUM_OF_CCE, sf_idx, cfi);
#if 0   //SISO
  formats = ue_siso_formats;
  nof_formats = nof_ue_siso_formats;
#endif

#if 1   //ALL Formats
  formats = falcon_ue_all_formats;
  nof_formats = nof_falcon_ue_all_formats;
#endif

  /* Calculate power levels on each cce*/
  srslte_pdcch_cce_avg_llr_power(&q->q->pdcch, cfi, cce_map, MAX_NUM_OF_CCE);

#if 0   //Enable/Disable minimum CCE Power
  /* Disable/remove locations which contain empty (low llr) CCEs */
  for(int cce_idx=0; cce_idx < q->pdcch.nof_cce; cce_idx++) {
      if(cce_map[cce_idx].avg_llr < DCI_MINIMUM_AVG_LLR_BOUND) {
          for(int l=0; l<4; l++) {
              srslte_dci_location_t* parent_location = cce_map[cce_idx].location[l];
              if(parent_location) {
                  parent_location->checked = true;
                  parent_location->used = false;
                }
            }
        }
    }
#endif


  q->q->current_rnti = 0xffff;
  bzero(q->colored_rb_map_dw, sizeof(((falcon_ue_dl_t *)0)->colored_rb_map_dw_bufA));
  bzero(q->colored_rb_map_up, sizeof(((falcon_ue_dl_t *)0)->colored_rb_map_up_bufA));
  q->totRBup = 0;
  q->totRBdw = 0;
  q->totBWup = 0;
  q->totBWdw = 0;

  gettimeofday(&t[1], NULL);

  for (uint32_t i=0;i<nof_locations;i++) {       // location loop
      for (uint32_t f=0;f<nof_formats;f++) {     // format loop
          if (!locations[i].checked && locations[i].sufficient_power) {
              int result = srslte_pdcch_decode_msg_limit_avg_llr_power(&q->q->pdcch, dci_msg, &locations[i], formats[f], cfi, &crc_rem, DCI_MINIMUM_AVG_LLR_BOUND);
              q->stats.nof_decoded_locations++;
              if(result != SRSLTE_SUCCESS) {
                  // DCI was dropped
                  //DEBUG("Dropped DCI due to too low LLR.\n");
                  continue;
                }

              //printf("t=%d.%d (%d (%d)), ret %d\n", sfn, sf_idx, locations[i].ncce, locations[i].L, ret);
              // Filter 1a: Impossible combinations
              // Format mismatch
              //if ((formats[f]==SRSLTE_DCI_FORMAT0 && dci_msg->data[0]==1) || (formats[f]==SRSLTE_DCI_FORMAT1A && dci_msg->data[0]==0)) continue;
              // Disallowed RNTI values for format 1c
              if ((formats[f]==SRSLTE_DCI_FORMAT1C) && (crc_rem > SRSLTE_RARNTI_END) && (crc_rem < SRSLTE_PRNTI)) continue;
              // Filter 1b: Illegal DCI positions
              if (!srslte_pdcch_validate_location(srslte_pdcch_nof_cce(&q->q->pdcch, cfi), locations[i].ncce, locations[i].L, sf_idx, crc_rem)) {
                  DEBUG("Dropped DCI cand. by illegal position: 0x%x\n", crc_rem);
                  continue;
              }
              // Histogram consideration
              rnti_histogram_add_rnti(&q->rnti_histogram[f], crc_rem);
              // Filter 2: Occurence Count
              if (rnti_histogram_get_occurence(&q->rnti_histogram[f], crc_rem) > RNTI_HISTOGRAM_THRESHOLD) {
                  locations[i].used = true;
                  //disable all remaining locations which contain CCEs covered by this accepted DCI
                  for(unsigned int covered_cce = locations[i].ncce; covered_cce < locations[i].ncce + (1 << locations[i].L); covered_cce++) {
                      for(int l=0; l<4; l++) {
                          falcon_dci_location_t* parent_location = cce_map[covered_cce].location[l];
                          if(parent_location) {
                              parent_location->checked = true;
                            }
                        }
                    }
                  srslte_ue_dl_reset_rnti_user(q, crc_rem);
                  //locations[i].checked = 1;
                  if (crc_rem >= SRSLTE_RARNTI_START
                      //    && crc_rem <= SRSLTE_CRNTI_END  // consider SI-RNTI, too
                      ) { // if ((crc_rem >= 0 && crc_rem <= 0x0010)) { //
                      //if (lprob[i] < PROB_THR) lprob[i]+=100;
                      if (
    #ifdef CNI_TIMESTAMP
                          srslte_dci_msg_to_trace_timestamp(dci_msg, crc_rem, q->q->cell.nof_prb, q->q->cell.nof_ports,
                                                            &dl_dci_unpacked, &ul_dci_unpacked, &dl_grant, &ul_grant, sf_idx, sfn, rnti_histogram_get_occurence(&q->rnti_histogram[f], crc_rem),
                                                            locations[i].ncce, locations[i].L, dci_msg->format, cfi, power, timestamp, rnti_histogram_get_occurence(&q->rnti_histogram[f], crc_rem), q->dci_file)
    #else
                          srslte_dci_msg_to_trace_timestamp(dci_msg, crc_rem, q->cell.nof_prb, q->cell.nof_ports,
                                                            &dl_dci_unpacked, &ul_dci_unpacked, &dl_grant, &ul_grant, sf_idx, sfn, lprob[i],
                                                            locations[i].ncce, locations[i].L, dci_msg->format, q->cfi, power)
    #endif
                          ) {
                          //fprintf(stderr,"1 Error unpacking DCI\n");
                        }
                      ret++;
                      // Check for RAR - set my rnti to discovered RA-RNTI, decode assigned T-RNTIs later
                      // if (crc_rem > 0x0000 && crc_rem <= 0x000a && formats[f]!=SRSLTE_DCI_FORMAT0) q->current_rnti = crc_rem;
                      // check upload/download
                      if (dl_grant.mcs[0].tbs>0) {
                          /* merge total RB map for RB allocation overview */
                          uint16_t color = crc_rem;
                          color = (((color & 0xFF00) >> 8) | ((color & 0x00FF) << 8));
                          color = (((color & 0xF0F0) >> 4) | ((color & 0x0F0F) << 4));
                          color = (((color & 0xCCCC) >> 2) | ((color & 0x3333) << 2));
                          color = (((color & 0xAAAA) >> 1) | ((color & 0x5555) << 1));
                          color = (color >> 1) + 16000;
                          for(uint32_t rb_idx = 0; rb_idx < q->q->cell.nof_prb; rb_idx++) {
                              if(dl_grant.prb_idx[0][rb_idx] == true) {
                                  if(q->colored_rb_map_dw[rb_idx] != 0) {
                                      q->collision_dw = true;
                                    }
                                  q->colored_rb_map_dw[rb_idx] = color;
                                }
                            }

                          q->totRBdw += dl_grant.nof_prb;
                          q->totBWdw += (uint32_t)(dl_grant.mcs[0].tbs + dl_grant.mcs[1].tbs);
                          if (q->totRBdw > q->q->cell.nof_prb) q->totBWdw = q->q->cell.nof_prb;
                        }
                      if (ul_grant.mcs.tbs>0) {
                          /* merge total RB map for RB allocation overview */
                          uint16_t color = crc_rem;
                          color = (((color & 0xFF00) >> 8) | ((color & 0x00FF) << 8));
                          color = (((color & 0xF0F0) >> 4) | ((color & 0x0F0F) << 4));
                          color = (((color & 0xCCCC) >> 2) | ((color & 0x3333) << 2));
                          color = (((color & 0xAAAA) >> 1) | ((color & 0x5555) << 1));
                          color = (color >> 1) + 16000;
                          for(uint32_t rb_idx = 0; rb_idx < ul_grant.L_prb; rb_idx++) {
                              if(q->colored_rb_map_up[ul_grant.n_prb[0] + rb_idx] != 0) {
                                  q->collision_up = true;
                                }
                              q->colored_rb_map_up[ul_grant.n_prb[0] + rb_idx] = color;
                            }

                          q->totRBup += ul_grant.L_prb;
                          q->totBWup += (uint32_t) ul_grant.mcs.tbs;
                          if (q->totRBup > q->q->cell.nof_prb) q->totBWup = q->q->cell.nof_prb;
                        }
                    }
                }
            } // checked
        } // format loop
    } // location loop

  gettimeofday(&t[2], NULL);
  get_time_interval(t);
  if (t[0].tv_usec > 800) printf("SF Scan took: %ld us\n", t[0].tv_usec);

  /* disabled validation loop
  for (int i=0;i<nof_locations;i++) {
      if (to_check[i] && !locations[i].checked && !locations[i].used) fprintf(stderr,"%d\t%d\t%d\t%d\t%d\n",sfn,sf_idx,locations[i].ncce,locations[i].L,q->cfi);
  }
  */

  if(ret == 0) {
      //printf("Empty Subframe!\n");
      //        q->colored_rb_map_dw[0] = 60000.0;
      //        q->colored_rb_map_up[0] = 60000.0;
    }
  //swap colored rb map buffers
  float* tmp_dw = q->colored_rb_map_dw_last;
  float* tmp_up = q->colored_rb_map_up_last;
  q->colored_rb_map_dw_last = q->colored_rb_map_dw;
  q->colored_rb_map_up_last = q->colored_rb_map_up;
  q->colored_rb_map_dw = tmp_dw;
  q->colored_rb_map_up = tmp_up;

  if(q->collision_dw) q->stats.nof_subframe_collisions_dw++;
  if(q->collision_up) q->stats.nof_subframe_collisions_up++;
  uint32_t missed = srslte_pdcch_nof_missed_cce(&q->q->pdcch, cfi, cce_map, MAX_NUM_OF_CCE);
  if(missed > 0) {
    fprintf(stderr, "Missed CCEs in SFN %d.%d: %d\n", sfn, sf_idx, missed);
  }
  q->stats.nof_missed_cce += missed;

  return ret;
}


int srslte_ue_dl_get_control_cc(falcon_ue_dl_t *q, uint32_t sf_idx, uint32_t sfn, bool reencoding_only)
{
  srslte_dci_msg_t dci_msg;
  int ret = SRSLTE_ERROR;
  uint32_t cfi;

  if ((ret = srslte_ue_dl_decode_fft_estimate_mbsfn(q->q, sf_idx, &cfi, SRSLTE_SF_NORM)) < 0) {
      return ret;
    }

  float noise_estimate = srslte_chest_dl_get_noise_estimate(&q->q->chest);

  if (srslte_pdcch_extract_llr_multi(&q->q->pdcch, q->q->sf_symbols_m, q->q->ce_m, noise_estimate, sf_idx, cfi)) {
      fprintf(stderr, "Error extracting LLRs\n");
      return SRSLTE_ERROR;
    }

#ifdef PRINT_SCAN_TIME
  struct timeval t[3];
  gettimeofday(&t[1], NULL);
#endif

  ret = srslte_ue_dl_find_dci_cc(q, &dci_msg, cfi, sf_idx, sfn, reencoding_only);

#ifdef PRINT_SCAN_TIME
  gettimeofday(&t[2], NULL);
  get_time_interval(t);
  printf("SF Scan took: %ld.%06ld s\n", t[0].tv_sec, t[0].tv_usec);
#endif

  q->stats.nof_subframes++;
#ifdef PRINT_LIVE_STATS
  srslte_ue_dl_stats_print(q);
#endif
  return ret;
}

int srslte_ue_dl_get_control_cc_hist(falcon_ue_dl_t *q, uint32_t sf_idx, uint32_t sfn)
{
  srslte_dci_msg_t dci_msg;
  int ret = SRSLTE_ERROR;
  uint32_t cfi;

  if ((ret = srslte_ue_dl_decode_fft_estimate_mbsfn(q->q, sf_idx, &cfi, SRSLTE_SF_NORM)) < 0) {
      return ret;
    }

  float noise_estimate = srslte_chest_dl_get_noise_estimate(&q->q->chest);

  if (srslte_pdcch_extract_llr_multi(&q->q->pdcch, q->q->sf_symbols_m, q->q->ce_m, noise_estimate, sf_idx, cfi)) {
      fprintf(stderr, "Error extracting LLRs\n");
      return SRSLTE_ERROR;
    }

#ifdef PRINT_SCAN_TIME
  struct timeval t[3];
  gettimeofday(&t[1], NULL);
#endif

#ifdef CNI_HISTOGRAM
  //ret = srslte_ue_dl_find_dci_cc(q, &dci_msg, q->cfi, sf_idx, SRSLTE_RNTI_USER, sfn);
  //ret = srslte_ue_dl_find_dci_histogram(q, &dci_msg, q->cfi, sf_idx, SRSLTE_RNTI_USER, sfn);
  //ret = srslte_ue_dl_recursive_blind_dci_search_old(q, &dci_msg, cfi, sf_idx, sfn);
  ret = srslte_ue_dl_recursive_blind_dci_search(q, &dci_msg, cfi, sf_idx, sfn);
#else
  ret = srslte_ue_dl_find_dci_cc(q, &dci_msg, cfi, sf_idx, sfn);
#endif

#ifdef PRINT_SCAN_TIME
  gettimeofday(&t[2], NULL);
  get_time_interval(t);
  printf("SF Scan took: %ld.%06ld s\n", t[0].tv_sec, t[0].tv_usec);
#endif

  q->stats.nof_subframes++;
#ifdef PRINT_LIVE_STATS
  srslte_ue_dl_stats_print(q);
#endif
  return ret;
}
