#include "graphene/plugins/witness_watchdog/witness_watchdog.hpp"
#include <graphene/chain/database.hpp>
#include <graphene/chain/witness_objects.hpp>
#include <graphene/chain/operation_notification.hpp>
#include <graphene/time/time.hpp>

#include <fc/io/json.hpp>
#include <fc/filesystem.hpp>
// #include <fc/network/http/connection.hpp> // Comentat: webhook
// #include <fc/network/url.hpp> // Comentat: webhook
// #include <fc/network/resolve.hpp> // Comentat: webhook

#include <boost/signals2/connection.hpp>

namespace graphene { namespace plugins { namespace witness_watchdog {

struct witness_watchdog_plugin::impl {
   graphene::plugins::chain::plugin& _chain_plugin;
   std::map<account_name_type, uint32_t> _missed_counts;
   std::map<account_name_type, uint32_t> _accumulated_misses;
   std::map<account_name_type, public_key_type> _signing_keys; // To track signing key changes
   std::set<account_name_type> _ignore_witnesses;
   fc::path _state_file;
   boost::signals2::scoped_connection _on_applied_block_connection;
   boost::signals2::scoped_connection _on_operation_connection;

   uint32_t _save_interval_blocks = 1000; // Intervalul de salvare a stării
   uint32_t _missed_threshold = 1; // Pragul de blocuri ratate pentru notificare
   // uint32_t _webhook_cooldown_sec = 300; // Comentat: webhook
   // std::map<account_name_type, fc::time_point> _last_witness_notification_time; // Comentat: webhook
   // std::string _webhook_url_str; // Comentat: webhook
   // bool _webhook_enabled = false; // Comentat: webhook

   impl() : _chain_plugin(appbase::app().get_plugin<graphene::plugins::chain::plugin>()) {}

   // Comentat: webhook
   // void send_webhook_notification(const std::string& message) {
   //    if (!_webhook_enabled || _webhook_url_str.empty()) return;
   //
   //    try {
   //       fc::url webhook_url(_webhook_url_str);
   //       fc::http::connection client;
   //       fc::http::request req;
   //       req.method = fc::http::post;
   //       req.host = webhook_url.host();
   //       req.port = webhook_url.port() != 0 ? webhook_url.port() : 80;
   //       req.path = webhook_url.path();
   //       req.add_header("Content-Type", "application/json");
   //       req.body = fc::json::to_string(fc::mutable_variant_object("text", message));
   //
   //       auto endpoints = fc::resolve(req.host, std::to_string(req.port));
   //       if (endpoints.empty()) return;
   //
   //       client.connect(endpoints[0]);
   //       client.send_request(req);
   //    } catch (const fc::exception& e) {
   //       wlog("[WATCHDOG] Eroare la trimiterea notificării webhook: ${e}", ("e", e.to_detail_string()));
   //    }
   // }
   //
   // void send_webhook_notification(const std::string& message, const account_name_type& witness_name) {
   //    if (_ignore_witnesses.count(witness_name)) return;
   //    if (_webhook_enabled && _webhook_cooldown_sec > 0) {
   //       if (_last_witness_notification_time.count(witness_name) && (fc::time_point::now() - _last_witness_notification_time[witness_name]).count() / 1000000 < _webhook_cooldown_sec) {
   //          return; // Cooldown activ
   //       }
   //       _last_witness_notification_time[witness_name] = fc::time_point::now();
   //    }
   //    send_webhook_notification(message);
   // }

   void on_applied_block(const signed_block& b) {
      auto& db = _chain_plugin.db();

      if (b.block_num() % _save_interval_blocks == 0)
         save_state();

      if (db.head_block_time() < graphene::time::now() - fc::seconds(CHAIN_BLOCK_INTERVAL * 2))
         return;

      const auto& witness_idx = db.get_index<witness_index>().indices().get<by_name>();
      int total_missed_this_check = 0;

      // Detectăm dacă martorul care a produs blocul curent și-a revenit
      if (!_ignore_witnesses.count(b.witness)) {
         if (_accumulated_misses[b.witness] > 0) {
            ilog("\033[32m[WATCHDOG] Witness ${w} is back online at block #${n}\033[0m", 
                 ("w", b.witness)("n", b.block_num()));
            // send_webhook_notification(fc::format_string("Witness ${w} is back online at block #${n}", // Comentat: webhook
            //      ("w", b.witness)("n", b.block_num()))); // Comentat: webhook
            _accumulated_misses[b.witness] = 0;
         }
      }

      // Alertă de latență (dacă blocul a fost produs cu întârziere mare)
      auto block_timestamp = b.timestamp;
      auto arrival_time = graphene::time::now();
      if (arrival_time - (fc::time_point)block_timestamp > fc::seconds(CHAIN_BLOCK_INTERVAL - 1)) {
         wlog("[WATCHDOG] Late block #${n} from ${w} (latency: ${l}ms)", 
              ("n", b.block_num())("w", b.witness)("l", (arrival_time - (fc::time_point)block_timestamp).count()/1000));
         // send_webhook_notification(fc::format_string("Late block #${n} from ${w} (latency: ${l}ms)", ("n", b.block_num())("w", b.witness)("l", (arrival_time - (fc::time_point)block_timestamp).count()/1000)), b.witness); // Comentat: webhook
      }

      for (const auto& witness : witness_idx) {
         if (_ignore_witnesses.count(witness.owner)) continue;

         auto it = _missed_counts.find(witness.owner);
         if (it != _missed_counts.end()) {
            if (witness.total_missed > it->second) {
               uint32_t diff = witness.total_missed - it->second;
               total_missed_this_check++;

               _accumulated_misses[witness.owner] += diff;
               if (_accumulated_misses[witness.owner] >= _missed_threshold) {
                  uint32_t acc = _accumulated_misses[witness.owner];
                  wlog("\033[31m[WATCHDOG] Witness ${w} a ratat ${n} bloc(uri) (acumulat)! Total ratate: ${t}\033[0m", 
                       ("w", witness.owner)("n", acc)("t", witness.total_missed));
                  // send_webhook_notification(fc::format_string("Witness ${w} a ratat ${n} bloc(uri) (acumulat)! Total ratate: ${t}", ("w", witness.owner)("n", acc)("t", witness.total_missed)), witness.owner); // Comentat: webhook
                  _accumulated_misses[witness.owner] = 0;
               }
            }
         }
         _missed_counts[witness.owner] = witness.total_missed;
      }

      if (total_missed_this_check >= 5) {
         wlog("\033[1;31m[WATCHDOG] ALERTA RETEA: ${n} martori au ratat blocuri simultan!\033[0m", ("n", total_missed_this_check));
         // send_webhook_notification(fc::format_string("ALERTA RETEA: ${n} martori au ratat blocuri simultan!", ("n", total_missed_this_check))); // Comentat: webhook
      }
   }

   void on_operation(const operation_notification& note) {
      const auto& db = _chain_plugin.db();
      if (db.head_block_time() < graphene::time::now() - fc::seconds(CHAIN_BLOCK_INTERVAL * 2))
         return;

      if (note.op.which() == operation::tag<witness_update_operation>::value) {
         const auto& op = note.op.get<witness_update_operation>();
         
         if (_ignore_witnesses.count(op.owner)) return;

         try {
            auto it_key = _signing_keys.find(op.owner);
            
            public_key_type old_signing_key = (it_key != _signing_keys.end() ? it_key->second : public_key_type());
            public_key_type new_signing_key;
            new_signing_key = op.block_signing_key; // block_signing_key este direct public_key_type, nu optional

            if (new_signing_key != old_signing_key) {
               ilog("\033[33m[WATCHDOG] Witness ${w} a schimbat cheia de semnare!\033[0m", ("w", op.owner));
               ilog("[WATCHDOG] Cheie veche: ${o}", ("o", old_signing_key));
               ilog("[WATCHDOG] Cheie noua: ${n}", ("n", new_signing_key));
               // send_webhook_notification(fc::format_string("Witness ${w} a schimbat cheia de semnare! Cheie veche: ${o}, Cheie noua: ${n}", ("w", op.owner)("o", old_signing_key)("n", new_signing_key)), op.owner); // Comentat: webhook
               _signing_keys[op.owner] = new_signing_key;
            }

            if (new_signing_key == public_key_type() && old_signing_key != public_key_type()) {
                wlog("\033[31m[WATCHDOG] ALERTA: Witness ${w} și-a setat cheia de semnare la NULL (dezactivat)!\033[0m", ("w", op.owner));
                // send_webhook_notification(fc::format_string("ALERTA: Witness ${w} și-a setat cheia de semnare la NULL (dezactivat)!", ("w", op.owner)), op.owner); // Comentat: webhook
            }
         } catch (...) {
            // Martorul poate fi nou creat
            _signing_keys[op.owner] = op.block_signing_key; // Stocăm direct cheia
         }
      } else if (note.op.which() == operation::tag<account_update_operation>::value) {
         const auto& op = note.op.get<account_update_operation>();
         const auto& witness_idx = db.get_index<witness_index>().indices().get<by_name>();

         if (_ignore_witnesses.count(op.account)) return;

         if (witness_idx.find(op.account) != witness_idx.end()) {
            ilog("\033[33m[WATCHDOG] Witness account ${a} updated authority (keys/metadata changed)!\033[0m", ("a", op.account));
            // send_webhook_notification(fc::format_string("Witness account ${a} updated authority (keys/metadata changed)!", ("a", op.account)), op.account); // Comentat: webhook
         }
      }
   }

   void save_state() {
      try {
         fc::mutable_variant_object state_obj;
         state_obj("missed_counts", _missed_counts);
         // state_obj("last_witness_notification_time", _last_witness_notification_time); // Comentat: webhook
         state_obj("accumulated_misses", _accumulated_misses);
         state_obj("signing_keys", _signing_keys);
         fc::json::save_to_file(state_obj, _state_file);
      } catch (const fc::exception& e) {
         wlog("[WATCHDOG] Nu am putut salva starea: ${e}", ("e", e.to_detail_string()));
      }
   }

   void load_state() {
      if (fc::exists(_state_file)) {
         try {
            fc::variant_object state_obj = fc::json::from_file(_state_file).get_object();
            _missed_counts = state_obj["missed_counts"].as<std::map<account_name_type, uint32_t>>();
            // if (state_obj.contains("last_witness_notification_time")) { // Comentat: webhook
            //    _last_witness_notification_time = state_obj["last_witness_notification_time"].as<std::map<account_name_type, fc::time_point>>(); // Comentat: webhook
            // }
            if (state_obj.contains("accumulated_misses")) {
               _accumulated_misses = state_obj["accumulated_misses"].as<std::map<account_name_type, uint32_t>>();
            }
            if (state_obj.contains("signing_keys")) { // Backward compatibility
               _signing_keys = state_obj["signing_keys"].as<std::map<account_name_type, public_key_type>>();
            }
         } catch (...) {
            wlog("[WATCHDOG] Fișierul de stare este corupt, se va reinițializa.");
         }
      }
   }
};

witness_watchdog_plugin::witness_watchdog_plugin() {}
witness_watchdog_plugin::~witness_watchdog_plugin() {}

void witness_watchdog_plugin::set_program_options(
   boost::program_options::options_description& cli,
   boost::program_options::options_description& cfg) {
   cli.add_options()
      ("witness-watchdog-save-interval", boost::program_options::value<uint32_t>()->default_value(1000), "Interval in blocks to save watchdog state")
      ("witness-watchdog-missed-threshold", boost::program_options::value<uint32_t>()->default_value(1), "Threshold of accumulated missed blocks before alert")
      ("witness-watchdog-ignore-witnesses", boost::program_options::value<std::vector<std::string>>()->multitoken(), "List of witness accounts to ignore")
      // ("witness-watchdog-webhook-cooldown", boost::program_options::value<uint32_t>()->default_value(300), "Cooldown in seconds for per-witness webhook notifications") // Comentat: webhook
      // ("witness-watchdog-webhook-url", boost::program_options::value<std::string>(), "Webhook URL for notifications (HTTP POST)") // Comentat: webhook
      ;
   cfg.add_options()
      ("witness-watchdog-save-interval", boost::program_options::value<uint32_t>()->default_value(1000), "Interval in blocks to save watchdog state")
      ("witness-watchdog-missed-threshold", boost::program_options::value<uint32_t>()->default_value(1), "Threshold of accumulated missed blocks before alert")
      ("witness-watchdog-ignore-witnesses", boost::program_options::value<std::vector<std::string>>()->multitoken(), "List of witness accounts to ignore")
      // ("witness-watchdog-webhook-cooldown", boost::program_options::value<uint32_t>()->default_value(300), "Cooldown in seconds for per-witness webhook notifications") // Comentat: webhook
      // ("witness-watchdog-webhook-url", boost::program_options::value<std::string>(), "Webhook URL for notifications (HTTP POST)") // Comentat: webhook
      ;
}

void witness_watchdog_plugin::plugin_initialize(const boost::program_options::variables_map& options) {
   my = std::make_unique<impl>();
   my->_state_file = appbase::app().data_dir() / "witness_watchdog_state.json";
   if (options.count("witness-watchdog-save-interval")) my->_save_interval_blocks = options["witness-watchdog-save-interval"].as<uint32_t>();
   if (options.count("witness-watchdog-missed-threshold")) my->_missed_threshold = options["witness-watchdog-missed-threshold"].as<uint32_t>();
   if (options.count("witness-watchdog-ignore-witnesses")) {
      auto witnesses = options["witness-watchdog-ignore-witnesses"].as<std::vector<std::string>>();
      for (const auto& w : witnesses) {
         my->_ignore_witnesses.insert(account_name_type(w));
      }
   }
   // if (options.count("witness-watchdog-webhook-url")) { // Comentat: webhook
   //    my->_webhook_url_str = options["witness-watchdog-webhook-url"].as<std::string>(); // Comentat: webhook
   //    my->_webhook_enabled = false; // Comentat: webhook
   // }
   // if (options.count("witness-watchdog-webhook-cooldown")) { // Comentat: webhook
   //    my->_webhook_cooldown_sec = options["witness-watchdog-webhook-cooldown"].as<uint32_t>(); // Comentat: webhook
   // }
}

void witness_watchdog_plugin::plugin_startup() {
   auto& db = my->_chain_plugin.db();
   
   // 1. Încărcăm starea salvată anterior (dacă există)
   my->load_state();

   // 2. Comparăm cu starea actuală din DB pentru a detecta ce s-a întâmplat în timpul offline-ului
   const auto& witness_idx = db.get_index<witness_index>().indices().get<by_name>();
   for (const auto& witness : witness_idx) {
      if (my->_ignore_witnesses.count(witness.owner)) continue;

      auto it = my->_missed_counts.find(witness.owner);
      if (it != my->_missed_counts.end()) {
         if (witness.total_missed > it->second) {
            uint32_t diff = witness.total_missed - it->second;
            if (diff >= my->_missed_threshold) {
               wlog("\033[31m[WATCHDOG] RESTART: Witness ${w} a ratat ${n} bloc(uri) cât timp nodul a fost oprit!\033[0m",
                    ("w", witness.owner)("n", diff));
               // my->send_webhook_notification(fc::format_string("RESTART: Witness ${w} a ratat ${n} bloc(uri) cât timp nodul a fost oprit!", ("w", witness.owner)("n", diff)), witness.owner); // Comentat: webhook
            }
         }
      }
      // Actualizăm cache-ul cu valorile curente din DB
      my->_missed_counts[witness.owner] = witness.total_missed;
      // Actualizăm cheia de semnare în cache
      my->_signing_keys[witness.owner] = witness.signing_key;
   }

   // 3. Salvăm imediat starea actualizată
   my->save_state();

   my->_on_applied_block_connection = db.applied_block.connect([this](const signed_block& b) {
      my->on_applied_block(b);
   });

   // Folosim pre_apply_operation pentru a vedea schimbarile inainte de a fi scrise in DB
   my->_on_operation_connection = db.pre_apply_operation.connect([this](const operation_notification& note) {
      my->on_operation(note);
   });

   ilog("Witness Watchdog plugin a pornit.");
   // if (my->_webhook_enabled) ilog("Notificările webhook sunt activate pentru URL: ${url}", ("url", my->_webhook_url_str)); // Comentat: webhook
}

void witness_watchdog_plugin::plugin_shutdown() {
   my->save_state();
   my->_on_applied_block_connection.disconnect();
   my->_on_operation_connection.disconnect();
}

} } } // graphene::plugins::witness_watchdog
