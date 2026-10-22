// Compile/link only: this program is never executed during dependency discovery.
#include <SDDS.h>

int main() {
  SDDS_DATASET data{};
  int32_t errors = 0;
  char description[] = "probe", filename[] = "probe.sdds";
  char name[] = "Time", units[] = "seconds";
  SDDS_ClearErrors();
  SDDS_InitializeOutput(&data, SDDS_BINARY, 1, description, description, filename);
  SDDS_DefineColumn(&data, name, nullptr, units, nullptr, nullptr, SDDS_DOUBLE, 0);
  SDDS_DefineParameter(&data, description, nullptr, nullptr, nullptr, nullptr, SDDS_STRING, description);
  SDDS_WriteLayout(&data);
  SDDS_StartPage(&data, 1);
  SDDS_SetParameters(&data, SDDS_SET_BY_INDEX | SDDS_PASS_BY_VALUE, int32_t{0}, description, int32_t{-1});
  double value = 0;
  SDDS_SetColumn(&data, SDDS_SET_BY_INDEX, &value, 1, int32_t{0});
  SDDS_WritePage(&data);
  SDDS_GetErrorMessages(&errors, SDDS_ALL_GetErrorMessages);
  return !SDDS_Terminate(&data);
}
