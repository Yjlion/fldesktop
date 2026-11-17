// CUPS helpers: names, media labels, device ids, state texts
#include "cupsclient.h"
#include "test.h"

int main() {
  using namespace prn;
  CHECK_EQ(queue_name("HP LaserJet 1020"), std::string("HP_LaserJet_1020"));
  CHECK_EQ(queue_name("Brother HL-L2350DW series"), std::string("Brother_HL-L2350DW_series"));
  CHECK_EQ(queue_name("EPSON ET-2810 (USB) / Kitchen"), std::string("EPSON_ET-2810_USB_Kitchen"));
  CHECK_EQ(queue_name("  "), std::string(""));
  CHECK_EQ(media_label("iso_a4_210x297mm"), std::string("A4"));
  CHECK_EQ(media_label("na_letter_8.5x11in"), std::string("Letter"));
  CHECK_EQ(media_label("custom_100x150mm_100x150mm"), std::string("100x150mm"));
  CHECK(is_driverless_uri("ipp://192.168.1.4/ipp/print"));
  CHECK(is_driverless_uri("ipps://printer.local:631/ipp/print"));
  CHECK(is_driverless_uri("dnssd://HP%20OfficeJet._ipp._tcp.local/?uuid=1"));
  CHECK(!is_driverless_uri("socket://192.168.1.4:9100"));
  CHECK(!is_driverless_uri("usb://HP/LaserJet%201020"));
  std::string id = "MFG:Hewlett-Packard;MDL:HP LaserJet 1020;CMD:PJL,ZJS;CLS:PRINTER;";
  CHECK_EQ(device_id_field(id, "MFG"), std::string("Hewlett-Packard"));
  CHECK_EQ(device_id_field(id, "MDL"), std::string("HP LaserJet 1020"));
  CHECK_EQ(device_id_field("MANUFACTURER:Brother;MODEL:HL-2030;", "MFG"), std::string("Brother"));
  CHECK_EQ(device_id_field(id, "SN"), std::string(""));
  Printer p;
  CHECK_EQ(printer_state_text(p), std::string("Ready"));
  p.state = 5;
  p.accepting = false;
  p.reasons = {"media-empty-error", "paused"};
  CHECK_EQ(printer_state_text(p), std::string("Paused, not accepting jobs, out of paper"));
  CHECK_EQ(job_state_text(9), std::string("Completed"));
  CHECK_EQ(job_state_text(8), std::string("Failed"));
  Job j;
  j.state = 5;
  CHECK(j.active());
  j.state = 9;
  CHECK(!j.active());
  return test_result("printers");
}
