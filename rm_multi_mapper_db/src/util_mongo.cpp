#include <util_mongo.h>

#include <algorithm>

using namespace mongo;
using namespace std;

util_mongo::util_mongo(string ip) {

	conn.connect(ip);
	cout << "connected to" << ip << endl;

	// Classes for feature extraction
	de = new cv::SurfDescriptorExtractor;
	fd = new cv::SurfFeatureDetector;

	fd->setInt("hessianThreshold", 400);
	fd->setInt("extended", 1);
	fd->setInt("upright", 1);

	de->setInt("hessianThreshold", 400);
	de->setInt("extended", 1);
	de->setInt("upright", 1);

}

util_mongo::~util_mongo() {
}

int util_mongo::getNextSequence(string name) {
	mongo::BSONObj cmdResult;
	bool ok = conn.runCommand(
	  "mapping",
	  BSON("findAndModify" << "counters"
	                  << "query" << BSON("_id"<<name)
	                  << "update" << BSON("$inc"<<BSON("seq"<<1))
	                  << "new" << true),
	  cmdResult);
	//cout<<cmdResult<<endl;
	return cmdResult["value"]["seq"].numberInt();
}

int util_mongo::get_new_robot_id() {

	int id = getNextSequence("robotid");
	mongo::BSONObjBuilder query;
	query.append("_id", id);
	query.append("map_id", id);
	conn.insert("mapping.robot", query.obj());
	string e = conn.getLastError();
	if (!e.empty()) {
		cout << "insert #1 failed: " << e << endl;
	}
	return id;

}

int util_mongo::get_mapid(int robot_id) {

	mongo::BSONObjBuilder query;
	query.append("map_id", robot_id);

	mongo::BSONObj res = conn.findOne("mapping.robot", query.obj());
	int map_id = res.getIntField("map_id");
	return map_id;

}

void util_mongo::add_keyframe(int robot_id, const color_keyframe::Ptr & k) {

	int map_id = get_mapid(robot_id);

	cout << "Map id " << map_id << endl;

	mongo::BSONObjBuilder query2;
	query2.appendNumber("_id", (long long) k->get_id());
	query2.appendNumber("q0", (double) k->get_pos().unit_quaternion().x());
	query2.appendNumber("q1", (double) k->get_pos().unit_quaternion().y());
	query2.appendNumber("q2", (double) k->get_pos().unit_quaternion().z());
	query2.appendNumber("q3", (double) k->get_pos().unit_quaternion().w());

	query2.appendNumber("t0", (double) k->get_pos().translation().x());
	query2.appendNumber("t1", (double) k->get_pos().translation().y());
	query2.appendNumber("t2", (double) k->get_pos().translation().z());

	query2.appendNumber("int0", (double) k->get_intrinsics()[0]);
	query2.appendNumber("int1", (double) k->get_intrinsics()[1]);
	query2.appendNumber("int2", (double) k->get_intrinsics()[2]);
	//srand(time(NULL));
	query2.appendNumber("random", (double) rand() / (RAND_MAX));

	std::vector<uint8_t> rgb_data, depth_data;

	cv::imencode(".png", k->get_rgb(), rgb_data);

	query2.appendBinData("rgb", rgb_data.size(), BinDataGeneral,
			(char*) rgb_data.data());

	cv::imencode(".png", k->get_d(0), depth_data);

	query2.appendBinData("depth", depth_data.size(), BinDataGeneral,
			(char*) depth_data.data());

	query2.append("map_id", map_id);
	cout << "Inserting" << endl;
	conn.insert("mapping.keyframe", query2.obj());
	string e = conn.getLastError();
	if (!e.empty()) {
		cout << "insert #2 failed: " << e << endl;
	}

}

void util_mongo::add_keypoints(const color_keyframe::Ptr & k) {
	BSONObjBuilder query;
	std::vector<cv::KeyPoint> keypoints;
	pcl::PointCloud<pcl::PointXYZ> keypoints3d;
	cv::Mat descriptors;
	compute_features(k->get_i(0), k->get_d(0), k->get_intrinsics(0), keypoints,
			keypoints3d, descriptors);

	query.append("num_keypoints", (int) keypoints3d.size());
	query.append("descriptor_size", (int) descriptors.cols);
	query.append("descriptor_type", (int) descriptors.type());

	//::assert(descriptors.type() == CV_32F);
	//std::cerr << "Keypoints size " << keypoints3d.size() << " "
	//		<< descriptors.size() << std::endl;

	query.appendBinData("descriptors",
			descriptors.cols * descriptors.rows * sizeof(float), BinDataGeneral,
			descriptors.data);
	query.appendBinData("keypoints",
			keypoints3d.points.size() * sizeof(pcl::PointXYZ), BinDataGeneral,
			keypoints3d.points.data());

	conn.update("mapping.keyframe", BSON("_id" << (long long)k->get_id()),
			BSON("$set" << query.obj()));
	string e = conn.getLastError();
	if (!e.empty()) {
		cout << "update #2 failed: " << e << endl;
	}
}

void util_mongo::add_measurement(long int first, long int second,
		const Sophus::SE3f & transform, const std::string & type) {
	/*INSERT INTO measurement"
	 " (`id`, `one`, `two`, `q0`, `q1`, `q2`,"
	 " `q3`, `t0`, `t1`, `t2`, `type`)"
	 " VALUES"
	 " (NULL,?,?,?,?,?,?,?,?,?,?*/
	BSONObjBuilder query;
	query.append("one", (long long) first);
	query.append("two", (long long) second);

	query.append("q0", (double) transform.unit_quaternion().x());
	query.append("q1", (double) transform.unit_quaternion().y());
	query.append("q2", (double) transform.unit_quaternion().z());
	query.append("q3", (double) transform.unit_quaternion().w());

	query.append("t0", (double) transform.translation().x());
	query.append("t1", (double) transform.translation().y());
	query.append("t2", (double) transform.translation().z());
	query.append("type", (string) type);
	conn.insert("mapping.measurement", query.obj());
	string e = conn.getLastError();
	if (!e.empty()) {
		cout << "insert #3 failed: " << e << endl;
	}
}

color_keyframe::Ptr util_mongo::get_keyframe(long frame_id) {

	BSONObjBuilder query;
	query.append("_id", (long long) frame_id);
	mongo::BSONObj res = conn.findOne("mapping.keyframe", query.obj());
	return get_keyframe(res);
}

void util_mongo::get_keyframe_ids(int map_id, std::vector<long long> & keyframes) {
	auto_ptr<DBClientCursor> cursor = conn.query("mapping.keyframe",
						QUERY("map_id" << map_id));
	while(cursor->more()) {
		mongo::BSONObj j = cursor->next();
		long long id = j["_id"].numberLong();
		keyframes.push_back(id);
	}

}

void util_mongo::get_keypoints(long frame_id,
		pcl::PointCloud<pcl::PointXYZ> & keypoints3d, cv::Mat & descriptors) {

	/*SELECT `keypoints`, `descriptors`, "
	 "`descriptor_size`, `num_keypoints`, `descriptor_type` "
	 "FROM keyframe WHERE `id` = ?*/
	//keypoints3d.points.data -> ? descriptors.data -> uchar *
	BSONObjBuilder query;
	query.append("_id", (long long) frame_id);
	mongo::BSONObj res = conn.findOne("mapping.keyframe", query.obj());

	keypoints3d.clear();
	//cout<<"frame_id"<<frame_id<<endl;
	int keypoints3d_buf_size;
	int keypoints3d_size;
	const pcl::PointXYZ * keypoints3d_ptr;
	keypoints3d_ptr = (pcl::PointXYZ *) res.getField("keypoints").binDataClean(
			keypoints3d_buf_size);
	keypoints3d_size = keypoints3d_buf_size / sizeof(pcl::PointXYZ);
	for (int i = 0; i < keypoints3d_size; i++) {
		keypoints3d.push_back(keypoints3d_ptr[i]);
	}

	int descriptors_size;
	const char *descriptors_ptr;
	descriptors_ptr = res.getField("descriptors").binDataClean(
			descriptors_size);

	int cols = res.getField("descriptor_size").numberDouble();
	int rows = res.getField("num_keypoints").numberDouble();
	int type = res.getField("descriptor_type").numberDouble();

	cv::Mat tmp_mat = cv::Mat(rows, cols, type, (void *) descriptors_ptr);

	//cv::Size s = tmp_mat.size();
	//cout<<"height width "<<s.height<<" "<<s.width<<endl;

	//cout << "M = "<< endl << " "  << tmp_mat << endl << endl;

	tmp_mat.copyTo(descriptors);

}

boost::shared_ptr<keyframe_map> util_mongo::get_robot_map(int robot_id) {
	int map_id = get_mapid(robot_id);
	auto_ptr<DBClientCursor> cursor = conn.query("mapping.keyframe",
			QUERY("map_id" << map_id));
	boost::shared_ptr<keyframe_map> map(new keyframe_map);

	while (cursor->more()) {
		map->frames.push_back(get_keyframe(cursor->next()));
	}

	return map;
}

Sophus::SE3f util_mongo::get_pose(mongo::BSONObj res) {
	Eigen::Quaternionf q;
	Eigen::Vector3f t;
	q.x() = res.getField("q0").numberDouble();
	q.y() = res.getField("q1").numberDouble();
	q.z() = res.getField("q2").numberDouble();
	q.w() = res.getField("q3").numberDouble();
	t[0] = res.getField("t0").numberDouble();
	t[1] = res.getField("t1").numberDouble();
	t[2] = res.getField("t2").numberDouble();

	return Sophus::SE3f(q, t);
}

color_keyframe::Ptr util_mongo::get_keyframe(BSONObj res) {

	Sophus::SE3f pose;
	pose = get_pose(res);

	Eigen::Vector3f intrinsics;

	intrinsics[0] = res.getField("int0").numberDouble();
	intrinsics[1] = res.getField("int1").numberDouble();
	intrinsics[2] = res.getField("int2").numberDouble();

	int rgb_size, depth_size;
	std::vector<uint8_t> rgb_data, depth_data;
	const char *rgb_ptr, *depth_ptr;
	rgb_ptr = res.getField("rgb").binDataClean(rgb_size);
	depth_ptr = res.getField("depth").binDataClean(depth_size);

	for (int i = 0; i < rgb_size; i++) {
		rgb_data.push_back((uint8_t) rgb_ptr[i]);
	}
	for (int i = 0; i < depth_size; i++) {
		depth_data.push_back((uint8_t) depth_ptr[i]);
	}

	cv::Mat rgb, depth;
	rgb = cv::imdecode(rgb_data, CV_LOAD_IMAGE_UNCHANGED);
	depth = cv::imdecode(depth_data, CV_LOAD_IMAGE_UNCHANGED);

	cv::Mat gray;
	cv::cvtColor(rgb, gray, CV_RGB2GRAY);

	color_keyframe::Ptr k(
			new color_keyframe(rgb, gray, depth, pose, intrinsics));

	k->set_id(res.getField("_id").numberLong());

	return k;
}

void util_mongo::get_overlapping_keyframes(long long frame_id, int map_id,
		std::vector<long> & overlapping_keyframes) {

	BSONObjBuilder query;
	query.append("_id", (long long) frame_id);
	mongo::BSONObj i = conn.findOne("mapping.keyframe", query.obj());
	long long i_id = i["_id"].numberLong();
	cout<<"frame_id "<<frame_id<<endl;
	auto_ptr<DBClientCursor> cursor2 = conn.query("mapping.keyframe",
					QUERY("map_id" << map_id));
	while(cursor2->more()) {

		mongo::BSONObj j = cursor2->next();
		long long j_id = j["_id"].numberLong();
		auto_ptr<DBClientCursor> cursor3 = conn.query("mapping.measurement",
					QUERY("one" << i_id << "two" << j_id));
		if(cursor3->more()) continue;
		if( i_id != j_id)
		{
			//cout<<"cursor3"<<endl;
			double iq0 = i["q0"].numberDouble();
			double iq1 = i["q1"].numberDouble();
			double iq2 = i["q2"].numberDouble();
			double iq3 = i["q3"].numberDouble();
			double jq0 = j["q0"].numberDouble();
			double jq1 = j["q1"].numberDouble();
			double jq2 = j["q2"].numberDouble();
			double jq3 = j["q3"].numberDouble();
			//cout<<iq0<<" "<<iq1<<endl;
			double it0 = i["t0"].numberDouble();
			double it1 = i["t1"].numberDouble();
			double it2 = i["t2"].numberDouble();
			double jt0 = j["t0"].numberDouble();
			double jt1 = j["t1"].numberDouble();
			double jt2 = j["t2"].numberDouble();

			float dotproduct = abs(iq0*jq0 + iq1*jq1 + iq2*jq2 + iq3*jq3);
			float angle = 2*acos(abs(iq0*jq0 + iq1*jq1 + iq2*jq2 + iq3*jq3));

			float distance = sqrt(pow((it0 - jt0), 2) +
								pow((it1 - jt1), 2) +
								pow((it2 - jt2), 2));

			if (((dotproduct) >= 1.0 || (angle < M_PI / 4)) && (distance < 3)) {

				overlapping_keyframes.push_back(j_id);
				//ROS_INFO(
				//		"Images %ld and %ld intersect with angular distance %f",
				//		i_id, j_id, angle*180/M_PI);
			}

		}
	}

	//ROS_INFO("Number of frame pairs %ld", overlapping_keyframes.size());

}

void util_mongo::load_measurements(long keyframe_id,
		std::vector<measurement> & m) {
	/*SELECT * FROM measurement "
	 "WHERE measurement.one = ?*/

	auto_ptr<DBClientCursor> cursor = conn.query("mapping.measurement",
			QUERY("one" << (long long)keyframe_id));

	while (cursor->more()) {
		BSONObj res = cursor->next();
		measurement mes;
		mes.first = res.getField("one").numberLong();
		mes.second = res.getField("two").numberLong();
		mes.transform = get_pose(res);
		m.push_back(mes);
	}

}

void util_mongo::load_positions(int map_id, std::vector<position> & p) {
	/*SELECT `keypoints`, `descriptors`, "
	 "`descriptor_size`, `num_keypoints`, `descriptor_type` "
	 "FROM keyframe WHERE `id` = ?"*/

	BSONObj projection =
			BSON("_id" << 1 << "q0" << 1 << "q1" <<
					1 << "q2" << 1 << "q3" << 1 <<
					"t0" << 1 << "t1" << 1 << "t2" << 1);
	auto_ptr<DBClientCursor> cursor = conn.query("mapping.keyframe",
			QUERY("map_id" << map_id), 0, 0, &projection);

	while (cursor->more()) {
		BSONObj res = cursor->next();
		position pos;
		pos.idx = res.getField("_id").numberLong();
		pos.transform = get_pose(res);
		p.push_back(pos);
	}

}

void util_mongo::update_position(const position & p) {
	/*UPDATE keyframe SET "
	 "`q0`= ?, `q1`= ?, `q2`= ?, `q3`= ?, "
	 "`t0`= ?, `t1`= ?, `t2`= ? WHERE id = ?*/

	BSONObjBuilder query;
	query.append("q0", (double) p.transform.unit_quaternion().x());
	query.append("q1", (double) p.transform.unit_quaternion().y());
	query.append("q2", (double) p.transform.unit_quaternion().z());
	query.append("q3", (double) p.transform.unit_quaternion().w());

	query.append("t0", (double) p.transform.translation().x());
	query.append("t1", (double) p.transform.translation().y());
	query.append("t2", (double) p.transform.translation().z());

	conn.update("mapping.keyframe", BSON("_id" << (long long)p.idx),
			BSON("$set" << query.obj()));
	string e = conn.getLastError();
	if (!e.empty()) {
		cout << "update #2 failed: " << e << endl;
	}

}

long long util_mongo::get_random_keyframe_idx(int map_id) {
	int random_number = (double) rand() / RAND_MAX;
	mongo::BSONObj res = conn.findOne("mapping.keyframe",
			QUERY("random"<<GTE<<random_number));
	return res.getField("_id").numberLong();

}

void util_mongo::merge_map(int old_map_id, int new_map_id) {
	conn.update("mapping.robot", BSON("map_id" << (int)old_map_id),
			BSON("$set" << BSON("map_id"<< (int)new_map_id)), false, true);
	string e = conn.getLastError();
	if (!e.empty()) {
		cout << "update #3 failed: " << e << endl;
	}

	conn.update("mapping.keyframe", BSON("map_id" << (int)old_map_id),
			BSON("$set" << BSON("map_id"<< (int)new_map_id)), false, true);
	e = conn.getLastError();
	if (!e.empty()) {
		cout << "update #4 failed: " << e << endl;
	}

}
