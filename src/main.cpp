#include <fstream>
#include <math.h>
#include <uWS/uWS.h>
#include <chrono>
#include <iostream>
#include <thread>
#include <vector>
#include "Eigen-3.3/Eigen/Core"
#include "Eigen-3.3/Eigen/QR"
#include "json.hpp"
#include "spline.h"

using namespace std;

// for convenience
using json = nlohmann::json;

// For converting back and forth between radians and degrees.
constexpr double pi() { return M_PI; }

double deg2rad(double x) { return x * pi() / 180; }

double rad2deg(double x) { return x * 180 / pi(); }

// In mph
const double MAX_VEL = 49.4;

// In mph
const double INC_VEL = 0.224;

// In meters
const double TOO_CLOSE_GAP = 30.0;

// Checks if the SocketIO event has JSON data.
// If there is data the JSON object in string format will be returned,
// else the empty string "" will be returned.
string hasData(string s) {

  auto found_null = s.find("null");
  auto b1 = s.find_first_of("[");
  auto b2 = s.find_first_of("}");

  if (found_null != string::npos) {
    return "";
  } else if (b1 != string::npos && b2 != string::npos) {
    return s.substr(b1, b2 - b1 + 2);
  }

  return "";
}

double distance(double x1, double y1, double x2, double y2) {
  return sqrt((x2 - x1) * (x2 - x1) + (y2 - y1) * (y2 - y1));
}

int ClosestWaypoint(double x, double y,
                    vector<double> maps_x, vector<double> maps_y) {

  double closestLen = 100000; //large number
  int closestWaypoint = 0;

  for (int i = 0; i < maps_x.size(); i++) {
    double map_x = maps_x[i];
    double map_y = maps_y[i];
    double dist = distance(x, y, map_x, map_y);

    if (dist < closestLen) {
      closestLen = dist;
      closestWaypoint = i;
    }
  }

  return closestWaypoint;
}

int NextWaypoint(double x, double y, double theta,
                 vector<double> maps_x, vector<double> maps_y) {

  int closestWaypoint = ClosestWaypoint(x, y, maps_x, maps_y);

  double map_x = maps_x[closestWaypoint];
  double map_y = maps_y[closestWaypoint];

  double heading = atan2((map_y - y), (map_x - x));
  double angle = abs(theta - heading);

  if (angle > pi() / 4) {
    closestWaypoint++;
  }

  return closestWaypoint;
}

// Transform from Cartesian x,y coordinates to Frenet s,d coordinates
vector<double> getFrenet(double x, double y, double theta,
                         vector<double> maps_x, vector<double> maps_y) {

  int next_wp = NextWaypoint(x, y, theta, maps_x, maps_y);
  int prev_wp;

  prev_wp = next_wp - 1;

  if (next_wp == 0) {
    prev_wp = maps_x.size() - 1;
  }

  double n_x = maps_x[next_wp] - maps_x[prev_wp];
  double n_y = maps_y[next_wp] - maps_y[prev_wp];
  double x_x = x - maps_x[prev_wp];
  double x_y = y - maps_y[prev_wp];

  // find the projection of x onto n
  double proj_norm = (x_x * n_x + x_y * n_y) / (n_x * n_x + n_y * n_y);
  double proj_x = proj_norm * n_x;
  double proj_y = proj_norm * n_y;

  double frenet_d = distance(x_x, x_y, proj_x, proj_y);

  // see if d value is positive or negative by comparing it to a center point
  double center_x = 1000 - maps_x[prev_wp];
  double center_y = 2000 - maps_y[prev_wp];
  double centerToPos = distance(center_x, center_y, x_x, x_y);
  double centerToRef = distance(center_x, center_y, proj_x, proj_y);

  if (centerToPos <= centerToRef) {
    frenet_d *= -1;
  }

  // calculate s value
  double frenet_s = 0;
  for (int i = 0; i < prev_wp; i++) {
    frenet_s += distance(maps_x[i], maps_y[i], maps_x[i + 1], maps_y[i + 1]);
  }

  frenet_s += distance(0, 0, proj_x, proj_y);

  return {frenet_s, frenet_d};
}

// Transform from Frenet s,d coordinates to Cartesian x,y
vector<double> getXY(double s, double d, vector<double> maps_s,
                     vector<double> maps_x, vector<double> maps_y) {
  int prev_wp = -1;

  while (s > maps_s[prev_wp + 1] && (prev_wp < (int) (maps_s.size() - 1))) {
    prev_wp++;
  }

  int wp2 = (prev_wp + 1) % maps_x.size();

  double heading = atan2((maps_y[wp2] - maps_y[prev_wp]),
                         (maps_x[wp2] - maps_x[prev_wp]));

  // the x,y,s along the segment
  double seg_s = (s - maps_s[prev_wp]);

  double seg_x = maps_x[prev_wp] + seg_s * cos(heading);
  double seg_y = maps_y[prev_wp] + seg_s * sin(heading);

  double perp_heading = heading - pi() / 2;

  double x = seg_x + d * cos(perp_heading);
  double y = seg_y + d * sin(perp_heading);

  return {x, y};
}

int main() {
  uWS::Hub h;

  // Load up map values for waypoint's x,y,s and d normalized normal vectors
  vector<double> map_waypoints_x;
  vector<double> map_waypoints_y;
  vector<double> map_waypoints_s;
  vector<double> map_waypoints_dx;
  vector<double> map_waypoints_dy;

  // Waypoint map to read from
  string map_file_ = "../data/highway_map.csv";

  // The max s value before wrapping around the track back to 0
  double max_s = 6945.554;

  ifstream in_map_(map_file_.c_str(), ifstream::in);

  string line;
  while (getline(in_map_, line)) {
    istringstream iss(line);
    double x;
    double y;
    float s;
    float d_x;
    float d_y;
    iss >> x;
    iss >> y;
    iss >> s;
    iss >> d_x;
    iss >> d_y;
    map_waypoints_x.push_back(x);
    map_waypoints_y.push_back(y);
    map_waypoints_s.push_back(s);
    map_waypoints_dx.push_back(d_x);
    map_waypoints_dy.push_back(d_y);
  }

  // Extra variables

  // Start in lane 1 (three lanes, starting from the left: 0, 1 and 2)
  int lane = 1;

  // Reference velocity to target in mph
  double ref_vel = 0;

  h.onMessage(
      [&ref_vel,
       &map_waypoints_x,
       &map_waypoints_y,
       &map_waypoints_s,
       &map_waypoints_dx,
       &map_waypoints_dy,
       &lane](
          uWS::WebSocket<uWS::SERVER> ws, char *data, size_t length, uWS::OpCode opCode) {
        // "42" at the start of the message means there's a websocket message event.
        // The 4 signifies a websocket message
        // The 2 signifies a websocket event
        //auto sdata = string(data).substr(0, length);
        //cout << sdata << endl;
        if (length && length > 2 && data[0] == '4' && data[1] == '2') {

          auto s = hasData(data);

          if (s != "") {
            auto j = json::parse(s);

            string event = j[0].get<string>();

            if (event == "telemetry") {
              // j[1] is the data JSON object

              // Main car's localization Data
              double car_x = j[1]["x"];
              double car_y = j[1]["y"];
              double car_s = j[1]["s"];
              double car_d = j[1]["d"];
              double car_yaw = j[1]["yaw"];
              double car_speed = j[1]["speed"];

              // Previous path data given to the Planner
              auto previous_path_x = j[1]["previous_path_x"];
              auto previous_path_y = j[1]["previous_path_y"];

              // Previous path's end s and d values
              double end_path_s = j[1]["end_path_s"];
              double end_path_d = j[1]["end_path_d"];

              // Sensor Fusion Data, a list of all other cars on the same
              // side of the road.
              auto sensor_fusion = j[1]["sensor_fusion"];

              // Debugging info
              cout << "= cars detected: " << sensor_fusion.size() << endl;
              cout << "= previous path: " << previous_path_x.size() << endl;

              json msgJson;

              // Define actual points for the planner
              vector<double> next_x_vals;
              vector<double> next_y_vals;

              // TODO: define a path made up of (x,y) points that the car will
              // visit sequentially every .02 seconds

              int prev_size = previous_path_x.size();

              if (prev_size > 0) {
                car_s = end_path_s;
              }

              // To avoid collision with other cars
              bool too_close = false;
              bool too_close_left = false;
              bool too_close_right = false;
              bool change_lane = false;
              bool car_ahead = false;

              // Find reference velocity to use
              for (int i = 0; i < sensor_fusion.size(); ++i) {

                float d = sensor_fusion[i][6];

                // If lane = 1
                //   in_same_lane  = d < 8 && d > 4
                //   in_left_lane = d < 4 && d > 0
                //   in_right_lane  = d > 8 && d < 12
                bool in_same_lane = d < (2 + 4 * lane + 2) && d > (2 + 4 * lane - 2);
                bool in_left_lane = d < (2 + 4 * lane - 2) && d > (2 + 4 * lane - 2 - 4);
                bool in_right_lane = d > (2 + 4 * lane + 2) && d < (2 + 4 * lane + 2 + 4) ;

                double vx = sensor_fusion[i][3];
                double vy = sensor_fusion[i][4];
                double check_speed = sqrt(vx * vx + vy * vy);
                double check_car_s = sensor_fusion[i][5];

                // Speed helps to predict where the car is going to be
                // Using previous points we can project s value in future
                check_car_s += (double)prev_size * 0.02 * check_speed;
                cout << "  = other car " << setw(2) << i
                     << "   s: " << fixed << setw(7) << setprecision(2) << check_car_s
                     << "   d: " << fixed << setw(5) << setprecision(2) << d
                     << "   speed: " << fixed << setw(5) << setprecision(2) << check_speed << endl;

                double gap = abs(check_car_s - car_s);

                // Other car is in my lane
                if (in_same_lane) {

                  // And it is ahead of us
                  if (check_car_s > car_s) {
                    // Only change lanes when we are not too close to the car
                    // in front
                    if (0.3 * TOO_CLOSE_GAP < gap && gap < 1.5 * TOO_CLOSE_GAP) {
                      change_lane = true;
                    }

                    // When we are within 30m of the car in front
                    if (gap < TOO_CLOSE_GAP) {
                      too_close = true;
                    }
                  }
                }

                if (in_left_lane) {
                  if ((check_car_s > car_s) &&
                      gap < 1.1 * TOO_CLOSE_GAP) {
                    too_close_left = true;
                  }

                  if (gap < 0.2 * TOO_CLOSE_GAP) {
                    too_close_left = true;
                  }

                  if (gap < 1.5 * TOO_CLOSE_GAP) {
                    car_ahead = true;
                  }
                }

                if (in_right_lane) {
                  if ((check_car_s > car_s) &&
                      gap < 1.1 * TOO_CLOSE_GAP) {
                    too_close_right = true;
                  }

                  if (gap < 0.2 * TOO_CLOSE_GAP) {
                    too_close_right = true;
                  }

                  if (gap < 1.5 * TOO_CLOSE_GAP) {
                    car_ahead = true;
                  }
                }
              }

              // Debugging info
              cout << "  autonomous car"
                   << "   s: " << fixed << setw(7) << setprecision(2) << car_s
                   << "   d: " << fixed << setw(5) << setprecision(2) << car_d
                   << "   speed: " << fixed << setw(5) << setprecision(2) << car_speed << endl;
              cout << "    = too close front: " << too_close << endl;
              cout << "    = too close left:  " << too_close_left << endl;
              cout << "    = too close right: " << too_close_right << endl;

              if (change_lane) {

                // State Machine
                switch (lane) {
                  case 0:
                    if (not too_close_right) {
                      lane = 1;
                    }
                    break;
                  case 1:
                    if (not too_close_left) {
                      lane = 0;
                    } else if (not too_close_right) {
                      lane = 2;
                    }
                    break;
                  case 2:
                    if (not too_close_left) {
                      lane = 1;
                    }
                    break;
                  default:
                    break;
                }
              } else {

                // Prefer staying in the center lane
                if (lane != 1 and not car_ahead) {
                  if (not too_close_left and not too_close_right) {
                    lane = 1;
                  }
                }
              }

              if (too_close) {
                // Rubric: The car does not exceed a total acceleration of
                // 10 m/s^2 and a jerk of 10 m/s^3.
                // A change in velocity of 0.224mph (1m/s) generates a total
                // acceleration of 5 m/s^2
                // 0.224 mph is about 0.1 m/s (1 meter/second = 2.24 mile/hour)
                ref_vel -= 0.5 * INC_VEL;
              } else if (ref_vel < MAX_VEL) {
                ref_vel += 2 * INC_VEL;
              }

              // Create a list of widely spaced (x, y) waypoints evenly spaced
              // at 30m.
              // Later we will interpolate these waypoints with a spline
              vector<double> pts_x;
              vector<double> pts_y;

              // Reference x, y, yaw states
              // For the starting point we will use either the car position or
              // the last points of the previous path
              double ref_x = car_x;
              double ref_y = car_y;
              double ref_yaw = deg2rad(car_yaw);

              // If the previous path is almost empty, use the car position
              if (prev_size < 2) {
                // Use two points that make the path tangent to the car
                double prev_car_x = car_x - cos(car_yaw);
                double prev_car_y = car_y - sin(car_yaw);

                pts_x.push_back(prev_car_x);
                pts_x.push_back(car_x);

                pts_y.push_back(prev_car_y);
                pts_y.push_back(car_y);

              } else {
                // Use the previous path end point as starting reference

                // Redefine reference state as previous path end point
                ref_x = previous_path_x[prev_size - 1];
                ref_y = previous_path_y[prev_size - 1];

                double ref_x_prev = previous_path_x[prev_size - 2];
                double ref_y_prev = previous_path_y[prev_size - 2];
                ref_yaw = atan2(ref_y - ref_y_prev, ref_x - ref_x_prev);

                pts_x.push_back(ref_x_prev);
                pts_x.push_back(ref_x);

                pts_y.push_back(ref_y_prev);
                pts_y.push_back(ref_y);
              }

              vector<double> next_wp0 = getXY(car_s + 30,
                                              (2 + 4 * lane),
                                              map_waypoints_s,
                                              map_waypoints_x,
                                              map_waypoints_y);
              vector<double> next_wp1 = getXY(car_s + 60,
                                              (2 + 4 * lane),
                                              map_waypoints_s,
                                              map_waypoints_x,
                                              map_waypoints_y);
              vector<double> next_wp2 = getXY(car_s + 90,
                                              (2 + 4 * lane),
                                              map_waypoints_s,
                                              map_waypoints_x,
                                              map_waypoints_y);

              pts_x.push_back(next_wp0[0]);
              pts_x.push_back(next_wp1[0]);
              pts_x.push_back(next_wp2[0]);

              pts_y.push_back(next_wp0[1]);
              pts_y.push_back(next_wp1[1]);
              pts_y.push_back(next_wp2[1]);


              // Convert from global to local car coordinates
              // Similar to the MPC project
              for (int i = 0; i < pts_x.size(); ++i) {
                // Translation
                double shift_x = pts_x[i] - ref_x;
                double shift_y = pts_y[i] - ref_y;

                // Rotation
                pts_x[i] = (shift_x * cos(0 - ref_yaw) - shift_y * sin(0 - ref_yaw));
                pts_y[i] = (shift_x * sin(0 - ref_yaw) + shift_y * cos(0 - ref_yaw));
              }


              // Create a spline
              tk::spline spl;

              // Prepare points
              spl.set_points(pts_x, pts_y);

              for (int i = 0; i < previous_path_x.size(); ++i) {
                next_x_vals.push_back(previous_path_x[i]);
                next_y_vals.push_back(previous_path_y[i]);
              }

              // Calculate how to break up spline points so we can travel at
              // our desired reference velocity
              double target_x = 30.0;
              double target_y = spl(target_x);
              double target_dist = sqrt(target_x * target_x + target_y * target_y);

              double x_add_on = 0;

              // Fill the points spacing the pints from the spline
              for (int i = 0; i < 50 - previous_path_x.size(); ++i) {

                // Convert from mph to m/s
                double N = target_dist/(0.02 * ref_vel / 2.24);
                double x_point = x_add_on + target_x / N;
                double y_point = spl(x_point);

                x_add_on = x_point;

                double x_ref = x_point;
                double y_ref = y_point;

                // Go back to global coordinates
                x_point = x_ref * cos(ref_yaw) - y_ref * sin(ref_yaw);
                y_point = x_ref * sin(ref_yaw) + y_ref * cos(ref_yaw);

                x_point += ref_x;
                y_point += ref_y;

                next_x_vals.push_back(x_point);
                next_y_vals.push_back(y_point);
              }

              msgJson["next_x"] = next_x_vals;
              msgJson["next_y"] = next_y_vals;

              auto msg = "42[\"control\"," + msgJson.dump() + "]";

              // this_thread::sleep_for(chrono::milliseconds(1000));
              ws.send(msg.data(), msg.length(), uWS::OpCode::TEXT);
            }
          } else {
            // Manual driving
            std::string msg = "42[\"manual\",{}]";
            ws.send(msg.data(), msg.length(), uWS::OpCode::TEXT);
          }
        }
      });

  // We don't need this since we're not using HTTP but if it's removed the
  // program doesn't compile :-(
  h.onHttpRequest([](uWS::HttpResponse *res, uWS::HttpRequest req, char *data, size_t, size_t) {
    const std::string s = "<h1>Hello world!</h1>";
    if (req.getUrl().valueLength == 1) {
      res->end(s.data(), s.length());
    } else {
      // i guess this should be done more gracefully?
      res->end(nullptr, 0);
    }
  });

  h.onConnection([&h](uWS::WebSocket<uWS::SERVER> ws, uWS::HttpRequest req) {
    std::cout << "Connected!!!" << std::endl;
  });

  h.onDisconnection([&h](uWS::WebSocket<uWS::SERVER> ws, int code, char *message, size_t length) {
    ws.close();
    std::cout << "Disconnected" << std::endl;
  });

  int port = 4567;

  if (h.listen(port)) {
    std::cout << "Listening to port " << port << std::endl;
  } else {
    std::cerr << "Failed to listen to port" << std::endl;
    return -1;
  }

  h.run();
}
