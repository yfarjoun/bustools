#include <iostream>
#include <fstream>
#include <algorithm>
#include <queue>
#include <functional>

#include "Common.hpp"
#include "BUSData.h"

#include "bustools_merge.h"

#define BP std::pair<BUSData, int>
#define TP std::pair<BUSData, int>

inline bool ncmp(const BP &a, const BP &b)
{
  if (a.first.flags == b.first.flags)
  {
    if (a.first.ec == b.first.ec)
    {
      return a.second > b.second;
    }
    else
    {
      return a.first.ec > b.first.ec;
    }
  }
  else
  {
    return a.first.flags > b.first.flags;
  }
}

// vocab
// txn: string representing transcript str
// tid: number indexing the transcript in the file
// ecs: the set of tids (set of transcript indexes)
// eid: the number indexing the equivalence class
// keep in mind this could be wrt original file or
// merged file

void bustools_merge_different_index(const Bustools_opt &opt)
{
  int nf = opt.files.size();
  std::vector<std::ifstream> bf(nf); // vector of bus file streams
  std::vector<BUSHeader> vh;         // vector of bus headers

  // populate the headers (we ignore ec for now)
  for (int i = 0; i < nf; i++)
  {
    bf[i].open((opt.files[i] + "/output.bus").c_str(), std::ios::binary);
    BUSHeader h;
    parseHeader(bf[i], h); // parse the header into h

    parseECs(opt.files[i] + "/matrix.ec", h); // parse ecs
    vh.push_back(std::move(h));               // place the ecs into h
  }
  std::cout << "[status] parsed BUS files" << std::endl;
  std::cout << "[status] parsed ECS files" << std::endl;

  // parse the transcripts.txt
  std::unordered_map<std::string, int32_t> txn_tid;
  std::vector<std::vector<int32_t>> tids_per_file; // list of tids as they occur for each file
  std::vector<int32_t> tids;                       // a vector of tids
  int32_t tid = 0;

  std::ofstream ofn(opt.output + "/transcripts.txt"); // master transcripts.txt

  // iterate through each file and populate txn_tid, tids perfile
  for (int i = 0; i < nf; i++)
  {
    tids.clear();
    std::ifstream ifn(opt.files[i] + "/transcripts.txt");
    std::string txn;

    while (ifn >> txn) // while still have transcript data
    {
      auto ok = txn_tid.insert({txn, tid}); // insert transcript and new index assoc with it
      if (ok.second)                        // if the insertion successful
      {
        tids.push_back(tid); // add the index to tids (this is a list of new index)
        ofn << txn << "\n";  // write to file
        tid += 1;            // increment transcript index
      }
      else
      {
        tids.push_back(ok.first->second); // this only hapens when the transcript appears in more than one busfile
      }
    }
    tids_per_file.push_back(tids); // new index
  }

  ofn.close();
  std::cout << "[status] transcripts.txt written" << std::endl;

  // all of the ecs are in the header
  // h.ecs is a vector<vector<ints>>
  // the first positional index is the equivalence eid
  // so h.ecs[eid_1] -> returns a vector of tids wrt local indexing
  BUSHeader oh;
  oh.version = BUSFORMAT_VERSION;
  oh.text = "Merged files";
  //TODO: parse the transcripts file, check that they are identical and merge.
  oh.bclen = vh[0].bclen;
  oh.umilen = vh[0].umilen;

  std::unordered_map<std::vector<int32_t>, int32_t, SortedVectorHasher> ecmapinv; // set{tids} (ec) to eid it came from
  for (int i = 0; i < tid; i++)
  {
    oh.ecs.push_back({i});
    ecmapinv.insert({{i}, i});
  }

  std::vector<std::vector<int32_t>> eids_per_file;
  std::vector<int32_t> eids;
  int32_t eid = ecmapinv.size();

  for (int i = 0; i < nf; i++)
  {
    eids.clear();

    BUSHeader h = vh[i];
    const auto &tids = tids_per_file[i]; // new index of tids for that file of the length of that file

    for (const auto &ecs : h.ecs) // ecs is a set of tids std::vector<int32_t>
    {
      std::vector<int32_t> new_ecs(ecs.size());
      if (ecs.size() == 1)
      {
        eid = tids[ecs[0]];
        eids.push_back(eid);
      }
      else if (ecs.size() > 1)
      {
        // convert tid to new coordinates
        for (int j = 0; j < ecs.size(); j++)
        {
          new_ecs[j] = tids[ecs[j]];
        }

        // check to see if the set exists in ecmapinv
        std::sort(new_ecs.begin(), new_ecs.end());
        auto it = ecmapinv.find(new_ecs); // see if new_ecs exists
        if (it != ecmapinv.end())
        {
          eid = it->second; // return the eid that it corresponds to
        }
        else
        {
          eid++;                     // make new eid
          oh.ecs.push_back(new_ecs); // add the set of tids (new ref)
          ecmapinv.insert({new_ecs, eid});
        }
        eids.push_back(eid);
      }
    }
    eids_per_file.push_back(std::move(eids));
  }

  // generate ecmap from ecmapinv
  std::vector<std::vector<int32_t>> ecmap(ecmapinv.size());
  for (const auto &ec : ecmapinv)
  {
    ecmap[ec.second] = ec.first; // eid -> ecs (set of tids)
  }

  // Process the busfiles
  std::ofstream outf(opt.output + "/merged.bus");
  writeHeader(outf, oh);
  writeECs(opt.output + "/raw.ec", oh); // prior to reading bus records

  size_t nr = 0, nw = 0;

  std::priority_queue<BP, std::vector<BP>, std::function<bool(const BP &a, const BP &b)>> pq(ncmp);
  BUSData bd;

  // insert first record from each busfile into pq
  for (int i = 0; i < nf; ++i)
  {
    if (bf[i].good())
    {
      bf[i].read((char *)&bd, sizeof(bd));
      if (bf[i].good())
      {
        pq.push({bd, i});
        ++nr;
      }
    }
  }

  BUSData prev = pq.top().first;

  std::unordered_set<int32_t> prev_eids;
  bool keep_record;
  while (!pq.empty())
  {
    keep_record = true;
    BP min = pq.top(); // top of heap
    pq.pop();          // remove min

    BUSData &m = min.first;
    int i = min.second;

    // check curr vs min
    if (m.flags == prev.flags && m.barcode == prev.barcode && m.UMI == prev.UMI)
    {
      prev_eids.insert(eids_per_file[i][prev.ec]); // insert new eid index
    }
    else
    {
      // only one eid in prev_eids
      if (prev_eids.size() == 1)
      {
        prev.ec = *prev_eids.begin();
      }
      else // merge the ecs and see if it exists, else add it
      {
        std::vector<int32_t> ecs;         // set of tids
        for (const auto &eid : prev_eids) // for each eid (from the bus records)
        {
          const auto &tids = ecmap[eid];                   // get the set of tids
          ecs.insert(ecs.end(), tids.begin(), tids.end()); // insert into ecs
        }
        std::sort(ecs.begin(), ecs.end());
        ecs.erase(std::unique(ecs.begin(), ecs.end()), ecs.end()); // keep only one of the duplicates
        auto it = ecmapinv.find(ecs);                              // see if the set exists

        if (it == ecmapinv.end()) // if it doesnt
        {
          keep_record = false;
          prev.ec = ecmapinv.size();       // make a new ec
          oh.ecs.push_back(ecs);           // add it to the matrix.ec
          ecmapinv.insert({ecs, prev.ec}); // insert into ecmapinv
          ecmap.push_back(ecs);            // add it to the ecmap
        }
        else // if it does exist
        {
          prev.ec = it->second;
        }
      }
      prev.count = 1;
      if (keep_record)
      {
        outf.write((char *)&prev, sizeof(prev));
        ++nw;
      }

      prev = m;
      prev_eids.clear();
      prev_eids.insert(eids_per_file[i][prev.ec]);
    }

    // Read in the next bus record into the queue
    if (bf[i].good())
    {
      bf[i].read((char *)&bd, sizeof(bd));
      if (bf[i].gcount() > 0)
      {
        pq.push({bd, i});
        ++nr;
      }
    }
  }

  // write out the remaining straggler
  if (prev_eids.size())
  {
    if (prev_eids.size() == 1)
    {
      prev.ec = *prev_eids.begin();
    }
    else // merge the ecs and see if it exists, else add it
    {
      std::vector<int32_t> ecs;         // set of tids
      for (const auto &eid : prev_eids) // for each eid (from the bus records)
      {
        const auto &tids = ecmap[eid];                   // get the set of tids
        ecs.insert(ecs.end(), tids.begin(), tids.end()); // insert into ecs
      }
      std::sort(ecs.begin(), ecs.end());
      ecs.erase(std::unique(ecs.begin(), ecs.end()), ecs.end()); // keep only one of the duplicates
      auto it = ecmapinv.find(ecs);                              // see if the set exists
      if (it == ecmapinv.end())                                  // if it doesnt
      {
        prev.ec = ecmapinv.size();       // make a new ec
        oh.ecs.push_back(ecs);           // add it to the matrix.ec
        ecmapinv.insert({ecs, prev.ec}); // insert into ecmapinv
        ecmap.push_back(ecs);            // add it to the ecmap
      }
      else // if it does exist
      {
        prev.ec = it->second;
      }
    }
    prev.count = 1;
    outf.write((char *)&prev, sizeof(prev));
    ++nw;
  }
  std::cout << "[status] merged.bus written" << std::endl;
  for (int i = 0; i < nf; ++i)
  {
    bf[i].close();
  }

  // write the master ec file
  writeECs(opt.output + "/matrix.ec", oh);
  std::cout << "[status] matrix.ec written" << std::endl;
  std::cerr << "BUS Records read:  " << nr << std::endl;
  std::cerr << "BUS Records wrote: " << nw << std::endl;
}

void bustools_merge(const Bustools_opt &opt)
{
  int k = opt.files.size();
  std::vector<std::ifstream> bf(k);
  std::vector<BUSHeader> vh;

  /* Parse all headers. */
  // TODO: check for compatible headers, version numbers umi and bclen
  for (int i = 0; i < k; ++i)
  {
    bf[i].open((opt.files[i] + "/output.bus").c_str(), std::ios::binary);
    BUSHeader h;
    parseHeader(bf[i], h);

    parseECs(opt.files[i] + "/matrix.ec", h);
    vh.push_back(std::move(h));
  }

  /* Parse all transcripts.txt files (and output new transcripts.txt file). */
  std::unordered_map<std::string, int32_t> txn_tid_map; // transcript, transcript_id map
  std::vector<std::vector<int32_t>> tids_per_file;
  std::vector<int32_t> tids;
  int32_t tid = 0;

  std::ofstream ofn(opt.output + "/transcripts.txt");

  for (int i = 0; i < k; ++i)
  {
    tids.clear();
    std::ifstream inf(opt.files[i] + "/transcripts.txt");
    std::string txp;
    while (inf >> txp) // read until no more transcripts
    {
      auto ok = txn_tid_map.insert({txp, tid}); // insert transcript and its id
      if (ok.second)                            // if insertion worked
      {
        tids.push_back(tid); // add the tid to list of tids
        ofn << txp << '\n';  // write out transcript to master list
        ++tid;
      }
      else // if the tid is already on the list
      {
        tids.push_back(ok.first->second); // push back the tid of the (look up what the pair is for insert)
      }
    }
    tids_per_file.push_back(tids); // add the list of tids for that file (correct combined indexing))
  }

  ofn.close();

  /* Create master ec */
  BUSHeader oh;
  oh.version = BUSFORMAT_VERSION;
  oh.text = "Merged files from BUStools";
  //TODO: parse the transcripts file, check that they are identical and merge.
  oh.bclen = vh[0].bclen;
  oh.umilen = vh[0].umilen;
  std::unordered_map<std::vector<int32_t>, int32_t, SortedVectorHasher> ecmapinv; // map of set of tids to the ecid
  std::vector<std::vector<int32_t>> ecid_per_file;                                // equivalence class ids per file
  std::vector<int32_t> ecids;                                                     // list of equivalence classes

  // oh.ecs = vh[0].ecs; // copy operator

  for (int32_t ecid = 0; ecid < tid; ecid++) // parse all of the single transcript ecs
  {
    ecids.push_back(ecid); // add the ecid
    // const auto &v = oh.ecs[ecid]; // get the transcripts corresponding to that ecid
    ecmapinv.insert({{ecid}, ecid}); // insert into ecmapinv the set of transcripts and the ecid
    oh.ecs.push_back({ecid});        // add the ecid to the ec
  }
  // ecid_per_file.push_back(std::move(ecids));

  for (int i = 0; i < k; i++)
  {
    ecids.clear();
    const auto &tids = tids_per_file[i];
    // merge the rest of the ecs
    for (const auto &v : vh[i].ecs)
    {
      if (v.size() > 1)
      {
        int32_t ec = -1;
        std::vector<int32_t> w(v.size());
        for (int j = 0; j < v.size(); ++j)
        {
          w[j] = tids[v[j]];
        }
        std::sort(w.begin(), w.end());
        auto it = ecmapinv.find(w);
        if (it != ecmapinv.end())
        {
          ec = it->second;
        }
        else
        {
          ec = ecmapinv.size();
          oh.ecs.push_back(w); // copy <- problem
          ecmapinv.insert({w, ec});
        }
        ecids.push_back(ec);
      }
      ecid_per_file.push_back(std::move(ecids));
    }
  }

  std::vector<std::vector<int32_t>> ecmap(ecmapinv.size());
  for (const auto &ec : ecmapinv)
  {
    ecmap[ec.second] = ec.first;
  }

  /* Process data. */
  std::ofstream outf(opt.output + "/output.bus");
  writeHeader(outf, oh);

  size_t nr = 0, nw = 0;
  std::priority_queue<TP, std::vector<TP>, std::function<bool(const TP &a, const TP &b)>> pq(ncmp);
  BUSData t;
  for (int i = 0; i < k; ++i)
  {
    if (bf[i].good())
    {
      bf[i].read((char *)&t, sizeof(t));
      if (bf[i].good())
      {
        pq.push({t, i});
        ++nr;
      }
    }
  }

  BUSData curr = pq.top().first;
  //  curr.count = 0; // We'll count this again in the first loop
  std::unordered_set<int32_t> currec;
  while (!pq.empty())
  {
    TP min = pq.top();
    pq.pop();

    BUSData &m = min.first;
    int i = min.second;
    // Do I have to check the other fields?
    if (m.flags == curr.flags && m.barcode == curr.barcode && m.UMI == curr.UMI)
    {
      currec.insert(ecid_per_file[i][m.ec]);
    }
    else
    {
      // Create new ec if necessary
      if (currec.size() == 1)
      {
        curr.ec = *currec.begin();
      }
      else
      {
        std::vector<int32_t> tx;
        for (const auto &ec : currec)
        {
          const auto &v = ecmap[ec];
          tx.insert(tx.end(), v.begin(), v.end());
        }
        std::sort(tx.begin(), tx.end());
        tx.erase(std::unique(tx.begin(), tx.end()), tx.end());

        auto it = ecmapinv.find(tx);
        if (it == ecmapinv.end())
        {
          curr.ec = ecmapinv.size();
          oh.ecs.push_back(tx); // Copy
          ecmapinv.insert({tx, curr.ec});
          ecmap.push_back(tx);
        }
        else
        {
          curr.ec = it->second;
        }
      }

      curr.count = 1;
      outf.write((char *)&curr, sizeof(curr));
      ++nw;

      curr = m;
      currec.clear();
      currec.insert(ecid_per_file[i][m.ec]);
    }

    // Read next
    if (bf[i].good())
    {
      bf[i].read((char *)&t, sizeof(t));
      if (bf[i].gcount() > 0)
      {
        pq.push({t, i});
        ++nr;
      }
    }
  }

  // Write out remaining straggler
  if (currec.size())
  {
    // Create new ec if necessary
    if (currec.size() == 1)
    {
      curr.ec = *currec.begin();
    }
    else
    {
      std::vector<int32_t> tx;
      for (const auto &ec : currec)
      {
        const auto &v = ecmap[ec];
        tx.insert(tx.end(), v.begin(), v.end());
      }
      std::sort(tx.begin(), tx.end());
      tx.erase(std::unique(tx.begin(), tx.end()), tx.end());

      auto it = ecmapinv.find(tx);
      if (it == ecmapinv.end())
      {
        curr.ec = ecmapinv.size();
        oh.ecs.push_back(tx); // Copy
        ecmapinv.insert({tx, curr.ec});
        ecmap.push_back(tx);
      }
      else
      {
        curr.ec = it->second;
      }
    }

    curr.count = 1;
    outf.write((char *)&curr, sizeof(curr));
    ++nw;
  }

  for (int i = 0; i < k; ++i)
  {
    bf[i].close();
  }

  /* Master ec file. */
  writeECs(opt.output + "/matrix.ec", oh);

  std::cerr << "Read in " << nr << " BUS records, wrote " << nw << " BUS records" << std::endl;
}
